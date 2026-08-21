#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_split.cpp — SHAKEOUT G implementation.  See kiwi_split.h for the
// Plasticity findings behind both verbs, the cut-plane derivation, the shared
// splitter's contract and the undo ordering (all read out of the sources, not
// assumed).
//
// NEW code over the ported cores.  Every brush that is created, linked or freed
// here goes through a ported function in the ported order; this file owns the
// plane derivation, the preview, the confirm flow and the §19 gate.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"                // camera_s
#include <gfx_d3d/r_gfx.h>          // GfxColor
#include <gfx_d3d/r_material.h>     // Material
#include <gfx_d3d/r_rendercmds.h>   // MaterialTechniqueType, TECHNIQUE_UNLIT

#include "kiwi_split.h"
#include "kiwi_camera.h"                    // ROUND X: KiwiCam_WorldPerPixel (the cut disc)
#include "kiwi_command.h"
// (ROUND L: kiwi_conselect.h is no longer included — Cut used to read the
//  construction SELECTION for its line and now picks one with the cursor, so the
//  store's own header is all this file needs.)
#include "kiwi_construct.h"
#include "kiwi_grid.h"                      // ROUND S: KiwiGrid_Snap (the §17 lattice)
#include "kiwi_lines.h"
#include "kiwi_material.h"                  // ROUND T: the inheritance rules
#include "kiwi_numeric.h"                   // ROUND S: the offset field
#include "kiwi_pick.h"
#include "kiwi_selection.h"
#include "kiwi_snap.h"                      // ROUND S: KiwiSnap_IsGeometry
#include "kiwi_units.h"                     // ROUND S: Units_ToDisplay (the HUD offset)
#include "kiwi_validity.h"
#include "kiwi_vec.h"     // KIWI-UX (CLEANUP, A-15): the one spelling of Dot3/Sub3/...

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>

// ── ported entry points (each verified against its definition) ──────────────
extern int         Sys_Printf( const char *fmt, ... );                        // win_qe3.cpp
extern camera_s   *Ed_Camera();                                               // camwnd.cpp
extern void        CamWnd_BuildMatrix();                                      // camwnd.cpp 0x403470
extern int         g_nUpdateBits;                                             // 0x25D5A74 (mainfrm.cpp)

extern void        Brush_SplitBrushByFace( brush_t *in, face_t *face,
                                           brush_t **front, brush_t **back );  // brush.cpp:4605 (0x471960)
extern selbrush_t *Brush_AddToList( brush_t *def, entity_s *owner );           // brush.cpp:669 (0x475980)
extern void        Brush_AddToList2( selbrush_t *b );                          // brush.cpp:927 (0x4765a0)
extern void        Brush_Free( selbrush_t *b );                                // brush.cpp:1002 (0x475ba0)
extern void        Brush_Free_R( brush_t *def );                               // brush.cpp:706 (0x475af0)
extern void        Entity_UnlinkBrush( brush_t *b );                           // entity.cpp:464 (0x485020)
extern void        Select_Deselect( int bAlsoFreeFaces );                      // select.cpp:1444 (0x48E800)
extern void        Select_Brush( selbrush_t *brush, char some_overwrite,
                                 char bStatus, char center_grid_on_selection ); // select.cpp:884

// (ROUND T: Ed_BuildClipFaceMaterial_Kiwi is no longer reached from here — the
//  template face's material now comes from kiwi_material.h's inheritance rules,
//  which fall back to that same forwarder for an all-tool brush.  See the fence
//  in KiwiSplit_DefByPlane.)

// The fill pair, the same two kiwi_region.cpp uses (kiwi_region.cpp:78 / :80-84).
extern char        Byte4PackPixelColor( float *from, GfxColor *out );          // 0x402ac0
extern void        __cdecl R_AddRenderCmdDrawTris(
                       Material *material, MaterialTechniqueType techType, short indexCount,
                       const uint16_t *indices, short vertexCount,
                       const float ( *xyzw )[4], const float ( *normal )[3], float *color,
                       const float ( *st )[2] );                               // 0x4fd1c0

namespace
{
    // Plasticity's cut phantom is red at 10% opacity (CutFactory.ts:333-343).  0.10
    // over a wall at KIWI's brightness reads as nothing at all, so the alpha is
    // raised to 0.22 — the same value kiwi_region.cpp's region fills use, chosen
    // there for exactly this reason.  The hue is Plasticity's, unchanged.
    const float KSPLIT_PLANE_RGBA[4] = { 1.00f, 0.16f, 0.16f, 0.22f };
    const float KSPLIT_LINE_COL[3]   = { 1.00f, 0.35f, 0.30f };   // the intersection outline
    const float KSPLIT_EDGE_COL[3]   = { 1.00f, 0.75f, 0.35f };   // the cut LINE itself
    // ROUND L: the stage-1 hover.  The SAME yellow every grabbable handle in the
    // editor uses (kiwi_gizmo.cpp KGZ_HOT, kiwi_lollipop.cpp KLOL_COL_BALL), so
    // "this is the thing the click will take" reads without being taught twice.
    const float KSPLIT_HOT_COL[3]    = { 1.00f, 0.90f, 0.30f };

    const float KSPLIT_EPS = 1.0e-4f;

    // ROUND S (the live face split).  KSPLIT_FACE_EPS is how close to the face
    // PLANE a geometry snap has to be to count as "on this face" — 0.1 world units,
    // the same order as the store's own weld tolerance, so a coplanar neighbour's
    // vertex is accepted (it really is on the plane) and anything in front or
    // behind is not.  KSPLIT_MIN_SPAN is the narrowest face the command will cut
    // and the guard band it keeps at each end of the slide, so neither half can
    // come back as a sliver the §19 gate then rejects.
    const float KSPLIT_FACE_EPS  = 0.1f;
    const float KSPLIT_MIN_SPAN  = 1.0f;

    // ── ROUND Y, ITEM 3: THE CENTRE SPOT ────────────────────────────────────
    // USER DIRECTIVE, verbatim: "when using the split tool, show the center dot
    // so I can find it easier."  The face CENTROID is where the cut starts (it is
    // Derive()'s `t = m_centreT` fallback and has been since round S) and it is
    // the one offset a modeller asks for by name — "split it exactly in half" —
    // but nothing drew it and nothing pulled the cursor to it.
    //
    // Two halves, and both are needed for "find it easier":
    //   * it is DRAWN, persistently, for the whole gesture, in kiwi_snap.cpp's own
    //     dot-and-ring glyph (KiwiSnap_EmitSpot) so it reads as a snap target
    //     rather than as a decoration.  The accent pass cannot do it — it is gated
    //     on a command that WantsClicks and this one drags (kiwi_snap.h).
    //   * it is SNAPPABLE.  The cursor's mapped offset latches onto the centroid
    //     inside KSPLIT_CENTRE_SNAP_PIX screen pixels, measured in PIXELS so the
    //     catch feels the same at every zoom — the same reasoning every capture
    //     radius in kiwi_snap.cpp uses.  8 px is the editor's own click slop
    //     (KBOX_CLICK_PIXELS, kiwi_boxselect.h) and is deliberately tighter than
    //     the 30 px axis guides, which own a whole line rather than one point.
    //   * Ctrl still suppresses it, because Ctrl suppresses snapping (§6) and a
    //     centre latch the user cannot escape is the defect, not the feature.
    const float KSPLIT_CENTRE_SNAP_PIX = 8.0f;
    // The centre spot's ink: the snap marker's language, but in the split's own
    // hot accent so it belongs to THIS gesture and cannot be mistaken for a
    // live snap result the marker has already landed on.
    const float KSPLIT_CENTRE_COL[3] = { 0.55f, 0.95f, 0.80f };

    // The sweep quad is sized off the affected brushes' bounds.  "Giant" in the
    // directive means "unmistakably crossing the solid"; a multiple of the bounds
    // radius does that at every zoom without becoming a map-wide sheet.
    const float KSPLIT_SPAN_SCALE  = 1.6f;    // half-length along the line
    const float KSPLIT_DEPTH_SCALE = 2.4f;    // sweep depth away from the camera


    // The plane through three points, as (normal, dist) with the interior on the
    // n·p <= d side of nothing in particular — this is only ever used for SIDE
    // TESTS, so the orientation does not matter as long as it is consistent.
    bool PlaneOf( const float p0[3], const float p1[3], const float p2[3],
                  float outN[3], float *outD )
    {
        float a[3], b[3];
        Sub3( p1, p0, a );
        Sub3( p2, p0, b );
        Cross3( a, b, outN );
        if ( !Norm3( outN ) )
            return false;
        *outD = Dot3( outN, p0 );
        return true;
    }

    // A brush instance this file may split: a real brush, not a patch, not a
    // fixed-size entity's.  Both tests are the ported cores' own — the clipper's
    // Ed_ProduceSplitLists de-selects exactly these two kinds (xywnd.cpp) and
    // CSG_MakeHollow skips them (csg.cpp).
    bool Splittable( const selbrush_t *b )
    {
        if ( !b || !b->def || b->patch )
            return false;
        const entity_s *owner = b->owner;
        if ( !owner || !owner->def )
            return false;
        const entity_s *ownerDef = owner->def;
        if ( !ownerDef->eclass || ownerDef->eclass->fixedsize )
            return false;
        return true;
    }

    // ── ROUND L: the construction SEGMENT under a pixel ─────────────────────
    // USER DIRECTIVE, verbatim: "The cut workflow is clunky.  It should be: Select
    // a solid, press C, then the selection expects a line to be selected."
    //
    // So the line is no longer a PRECONDITION read out of the construction
    // selection (shakeout G's CutLine, which required exactly one construction item
    // to be selected before C was pressed at all) — it is picked with the cursor
    // INSIDE the gesture, the same way Match Face picks its target face.
    //
    // WHY NOT KiwiConSel_PickAt, which is the file that owns construction picking.
    // Same reason Match Face re-casts its own ray (kiwi_matchface.cpp): that entry
    // point resolves at the granularity the CURRENT SELECTION MODE asks for
    // (KindForMode), and in Object / Face / All mode it answers KCONSEL_OBJECT with
    // index -1 — a whole circle, with no segment named.  Cut needs ONE segment
    // whatever mode the user happens to be in, so it scans segments directly.  The
    // TOLERANCE is the shared one, KCON_LINE_PIXELS (10 px, kiwi_construct.h), so
    // the clickbox is exactly the one every other construction pick uses.
    //
    // ── ROUND T: …AND A BRUSH FACE OR A BRUSH EDGE ──────────────────────────
    // USER DIRECTIVE, verbatim: "I want a new addition to the cut tool.  Make it
    // so you can select faces from brushes as well.  Imagine clicking the roof on
    // another brush and using it as a plane to cut.  That's what I mean.  Allow
    // Planes AND lines to be selected (and even edges, why not)."
    //
    // So stage 1 now accepts THREE kinds of pick, and each answers "what is the
    // cutting plane" differently:
    //
    //   KCUT_SRC_LINE   a construction segment.  The plane passes through it and
    //                   sweeps AWAY FROM THE CAMERA — the shakeout-G derivation,
    //                   unchanged, because a line names a plane only together
    //                   with a view direction.
    //   KCUT_SRC_EDGE   a brush edge.  IDENTICAL treatment: an edge is a line
    //                   that happens to belong to a solid, and "why not" is
    //                   exactly right — the derivation already exists and the
    //                   only new thing is where the two endpoints came from.
    //   KCUT_SRC_FACE   a brush face.  ITS PLANE **IS** THE CUTTING PLANE.  No
    //                   camera term at all: a face already carries a normal and a
    //                   distance, which is the whole of a plane, so deriving one
    //                   from the view would be throwing information away.  This
    //                   is "click the roof on another brush and cut with it".
    //
    // ANOTHER brush is guaranteed rather than hoped for: Cut's PickFlags are
    // PICKF_EXCLUDE_SELECTED, and the brushes being cut ARE the selection, so the
    // pick chain cannot return a face or an edge of a target (kiwi_pick.h).  The
    // gesture therefore cannot cut a brush along its own face, which would be a
    // no-op with a preview.
    enum cutSrc_t { KCUT_SRC_LINE = 0, KCUT_SRC_EDGE, KCUT_SRC_FACE };

    struct cutPick_t
    {
        cutSrc_t kind    = KCUT_SRC_LINE;
        int   object = -1;                      // construction store index (LINE)
        int   segment = -1;
        selbrush_t *node = 0;                   // brush instance (EDGE / FACE)
        int   faceIndex  = -1;
        float a[3] = { 0.0f, 0.0f, 0.0f };      // LINE / EDGE: the two endpoints
        float b[3] = { 0.0f, 0.0f, 0.0f };
        float n[3] = { 0.0f, 0.0f, 1.0f };      // FACE: its plane, as (n, d)
        float d    = 0.0f;
        float dist = 0.0f;                      // pixels from the cursor (FACE: 0)
    };

    // ROUND AF, ITEM 1: the hard ceiling on a fence.  A cut by N planes can, in
    // the worst case, produce 2^N solids, and 64 is already 2^64 of theoretical
    // headroom that real geometry never approaches (a circle cutting a slab makes
    // exactly 2 pieces, not 2^32 — each plane only divides the pieces it actually
    // crosses).  It is the same 64 KCON_SEGS_MAX caps a circle at, so a maximally
    // tessellated ring fits exactly and nothing the construction layer can draw
    // is refused for being too fine.
    const int KCUT_MAX_FENCE = 64;

    // KIWI-UX (CLEANUP, PickLineAt): the scan moved to KiwiCon_PickSegmentAt
    // (kiwi_construct.h) — kiwi_dupe.cpp had a verbatim second copy of it under a
    // different payload.  What stays here is the payload: the cut's cutPick_t,
    // which is ranked against a brush FACE and a brush EDGE pick a few lines
    // below and so has to carry a kind and a pixel distance.
    bool PickLineAt( int imgX, int imgY, cutPick_t *out )
    {
        if ( !out )
            return false;
        cutPick_t best;
        if ( !KiwiCon_PickSegmentAt( imgX, imgY, best.a, best.b,
                                     &best.object, &best.segment, &best.dist ) )
            return false;
        best.kind = KCUT_SRC_LINE;
        *out = best;
        return true;
    }

    // ── ROUND T: the BRUSH half of stage 1 ──────────────────────────────────
    // One ordinary pick through the shared chain, asked for EDGES and FACES only.
    // Pick() already resolves point → line → area in that order at the shared
    // pixel tolerances (kiwi_pick.h), so "the edge wins over the face it lies on"
    // is not a rule this file has to invent — it is the pick API's own, the same
    // one the marquee and the hover use, which is why an edge and a face hover
    // exactly where the user expects from every other tool.
    //
    // SEL_VERTEX is deliberately NOT in the mask: a point names no plane, and
    // admitting it would only steal the pixel from the edge that owns it.
    bool PickBrushAt( int imgX, int imgY, cutPick_t *out )
    {
        if ( !out )
            return false;
        ray_t ray;
        if ( !Pick_RayFromImagePos( imgX, imgY, &ray ) )
            return false;

        const pick_result_t r = Pick( ray, SEL_MASK_EDGE | SEL_MASK_FACE,
                                      PICKF_EXCLUDE_SELECTED );
        if ( !r.valid || !Sel_BrushLive( r.item.brush ) || r.item.brush->patch )
            return false;                       // a patch has no half-space to lend
        const brush_t *def = r.item.brush->def;
        if ( !def || !def->faces )
            return false;
        const int fi = r.item.faceIndex;
        if ( fi < 0 || fi >= def->faceCount )
            return false;
        const face_t *f = &def->faces[fi];

        if ( r.item.kind == SEL_EDGE )
        {
            const winding_t *w = f->w;
            if ( !w || w->numpoints < 2 || w->numpoints > MAX_POINTS_ON_WINDING )
                return false;
            const int e = r.item.edgeIndex;
            if ( e < 0 || e >= w->numpoints )
                return false;
            out->kind      = KCUT_SRC_EDGE;
            out->node      = r.item.brush;
            out->faceIndex = fi;
            Copy3( w->p[e], out->a );
            Copy3( w->p[( e + 1 ) % w->numpoints], out->b );
            out->dist = r.screenDist;
            return true;
        }

        // FACE — its plane, read off the face and re-derived from the winding so
        // the distance is exact for THIS winding rather than for whatever the
        // planepts happened to be before the last rebuild.
        const winding_t *w = f->w;
        if ( !w || w->numpoints < 3 || w->numpoints > MAX_POINTS_ON_WINDING )
            return false;
        float n[3];
        Copy3( f->plane.normal, n );
        if ( !Norm3( n ) )
            return false;
        out->kind      = KCUT_SRC_FACE;
        out->node      = r.item.brush;
        out->faceIndex = fi;
        Copy3( n, out->n );
        out->d    = Dot3( n, w->p[0] );
        out->dist = r.screenDist;
        return true;
    }

    // The stage-1 pick, all three kinds, ranked.  A construction segment and a
    // brush edge are both PIXEL hits and compete on pixel distance, with the
    // construction line winning an exact tie (it is Cut's documented primary and
    // the thing the console line asks for).  A FACE is an AREA hit — screenDist 0
    // by construction (kiwi_pick.h) — so it can never out-rank a line-like hit;
    // it is what you get when neither is under the cursor, which is exactly the
    // "click the roof" gesture.
    bool PickCutSourceAt( int imgX, int imgY, cutPick_t *out )
    {
        cutPick_t linePick, brushPick;
        const bool haveLine  = PickLineAt ( imgX, imgY, &linePick  );
        const bool haveBrush = PickBrushAt( imgX, imgY, &brushPick );

        if ( haveLine && haveBrush )
        {
            if ( brushPick.kind == KCUT_SRC_FACE || brushPick.dist > linePick.dist )
            { *out = linePick;  return true; }
            *out = brushPick;  return true;
        }
        if ( haveLine )  { *out = linePick;  return true; }
        if ( haveBrush ) { *out = brushPick; return true; }
        return false;
    }

    // Is there ANY pickable construction segment in the store?
    //
    // ROUND T: this is no longer a PRECONDITION — a map with no construction
    // geometry at all is now a perfectly good place to run Cut, because a brush
    // face or a brush edge answers the same question.  It survives only to choose
    // the wording of the opening console line, so the prompt names the thing the
    // user can actually click.
    bool AnyConstructionSegment()
    {
        if ( !KiwiCon_ShowConstruction() )
            return false;
        const int count = KiwiCon_Count();
        for ( int i = 0; i < count; ++i )
        {
            const kconObject_t *o = KiwiCon_At( i );
            if ( o && KiwiCon_SegmentCount( *o ) > 0 )
                return true;
        }
        return false;
    }

    // ── the translucent sweep quad ──────────────────────────────────────────
    // Its own MATERIAL_COLOR bracket, the ported selected-face fill's
    // (camwnd.cpp 0x408106, and kiwi_region.cpp:661-666 spells it out): neutral so
    // the per-vertex colour drives the draw, white again afterwards.
    // ── KIWI-UX (ROUND X, ITEM 7): THE CUT DISC ─────────────────────────────
    // USER DIRECTIVE, verbatim: "The cut previewer is pretty good, but I would like
    // you to make it a semi-transparent circle about 250% bigger than the area
    // we're cutting."
    //
    // The previewer the directive is looking at is the snap marker's dot-and-ring
    // (kiwi_snap.cpp EmitDotAndRing — KSNAP_DOT_PIX 2 px filled, KSNAP_RING_PIX
    // 6 px outline), which is what marks the point a click would cut at.  So the
    // ring's 6 px is "the area we're cutting" and 250% of it is 15 px.  The dot and
    // the ring both STAY — they are the precise mark and the directive calls the
    // existing previewer good; the disc is added UNDER them as the area readout.
    //
    // A filled, genuinely translucent disc, so it needs the triangle path rather
    // than kiwi_lines (which pins alpha to 1 — kiwi_lines.h TRAP 2).  It is the
    // same R_AddRenderCmdDrawTris + MATERIAL_COLOR bracket DrawQuad below and
    // kiwi_region.cpp's region fills already use.
    // KIWI-UX (CLEANUP, A-29): this WAS `15.0f  // 2.5x KSNAP_RING_PIX
    // (kiwi_snap.cpp:74)` — a copied number with an already-stale cite, in a file
    // that uses the published accessors correctly 1900 lines further down
    // (:2383).  kiwi_snap.h publishes KiwiSnap_RingPixels() precisely "so a
    // caller can match them exactly rather than copying numbers that then
    // drift", so only the RATIO lives here now and the radius is computed at the
    // use site.
    const float KSPLIT_DISC_SCALE = 2.5f;   // of KiwiSnap_RingPixels() — see above
    const int   KSPLIT_DISC_SEGS = 24;      // reads round at 15 px; 22 indices*3
    // The HOVER accent, at the sweep quad's own alpha, so the two previews read as
    // one language.  KSPLIT_HOT_COL + KSPLIT_PLANE_RGBA[3].
    const float KSPLIT_DISC_RGBA[4] = { 1.00f, 0.90f, 0.30f, 0.22f };

    // A translucent fan in the plane (`centre`, `normal`).  `normal` may be null,
    // in which case the disc faces the camera — a construction line and a brush
    // edge have no surface to lie on, and a disc edge-on to the view is invisible.
    void DrawDisc( const float centre[3], const float *normal, float radius,
                   const float rgba[4] )
    {
        static const float s_neutral[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        static const float s_white[4]   = { 1.0f, 1.0f, 1.0f, 1.0f };

        // KIWI-UX (CLEANUP, A-21): no null test — Ed_Camera never returns NULL
        // (camwnd.cpp:159); only the degenerate radius is a real refusal.
        const camera_s *cam = Ed_Camera();
        if ( !( radius > 0.0f ) )
            return;

        float n[3];
        if ( normal )
            Copy3( normal, n );
        else
            for ( int k = 0; k < 3; ++k ) n[k] = -cam->vpn[k];
        if ( !Norm3( n ) )
            return;

        // ── ROUND Y, ITEM 8: THE DISC IS BACKFACE-CULLED ────────────────────
        // USER REPORT, verbatim: "the cut previewer I asked you to add only
        // renders when i'm underneath the grid.  Fix that."
        //
        // "Visible from one side only" is backface culling on a fixed winding, and
        // the state says so: this fan is drawn with g_qeglobals.d_white =
        // Material_RegisterHandle("white_tools") (gfxwrapper.cpp:77), and
        // main/materials/white_tools carries refStateBits[0] = 0x08128965, whose
        // cull field (0x08128965 & GFXS0_CULL_MASK 0xC000 = 0x8000) is
        // GFXS0_CULL_BACK -> s_cullTable_30[2] = 3 = D3DCULL_CCW (r_state.cpp:28,
        // :919).  main/statemaps/default.sm passes cullFace through, so the state
        // survives to the device.  There is nothing to fix in the material — the
        // fan simply has to be wound toward the eye.
        //
        // The fix is one sign.  The ring below runs +u then +v with v = n x u, so
        // its front face is the +n side; making n point AT the camera therefore
        // makes the front face the one being looked at, from either side of a
        // surface.  It is also exactly what the null-normal branch above already
        // does (n = -vpn points at the eye), which is why the camera-facing discs
        // — a line hover, an edge hover — always drew and only the FACE-oriented
        // ones went missing.  The disc still lies IN the surface plane; only its
        // winding changes.
        {
            float toEye[3];
            for ( int k = 0; k < 3; ++k )
                toEye[k] = cam->origin[k] - centre[k];
            if ( Dot3( n, toEye ) < 0.0f )
                for ( int k = 0; k < 3; ++k ) n[k] = -n[k];
        }

        // An orthonormal pair in the plane, seeded from the world axis least
        // aligned with the normal — the standard trick, and the same one
        // KiwiCon_MakePlane uses, so a disc on an axis-aligned face is axis-aligned.
        float seed[3] = { 0.0f, 0.0f, 0.0f };
        {
            int least = 0;
            for ( int k = 1; k < 3; ++k )
                if ( fabsf( n[k] ) < fabsf( n[least] ) )
                    least = k;
            seed[least] = 1.0f;
        }
        float u[3], v[3];
        const float dn = Dot3( seed, n );
        for ( int k = 0; k < 3; ++k )
            u[k] = seed[k] - n[k] * dn;
        if ( !Norm3( u ) )
            return;
        Cross3( n, u, v );
        if ( !Norm3( v ) )
            return;

        // Toward the eye by the region fill's own nudge, for the region fill's own
        // reason: a disc drawn ON a brush face is otherwise decided pixel-by-pixel
        // against that face's depth (kiwi_region.h KREG_FILL_NUDGE).
        const float nudge = 0.5f;

        float          xyzw[KSPLIT_DISC_SEGS + 1][4];
        float          nrm [KSPLIT_DISC_SEGS + 1][3];
        float          st  [KSPLIT_DISC_SEGS + 1][2];
        float          col [KSPLIT_DISC_SEGS + 1];
        uint16_t       idx [KSPLIT_DISC_SEGS * 3];

        float rgbaCopy[4] = { rgba[0], rgba[1], rgba[2], rgba[3] };
        GfxColor packed;
        Byte4PackPixelColor( rgbaCopy, &packed );
        const float packedAsFloat = *(float *)&packed.packed;

        for ( int i = 0; i <= KSPLIT_DISC_SEGS; ++i )
        {
            float p[3];
            if ( i == 0 )
            {
                Copy3( centre, p );
            }
            else
            {
                const float a = ( 6.28318530718f * (float)( i - 1 ) )
                              / (float)KSPLIT_DISC_SEGS;
                const float cs = cosf( a ) * radius;
                const float sn = sinf( a ) * radius;
                for ( int k = 0; k < 3; ++k )
                    p[k] = centre[k] + u[k] * cs + v[k] * sn;
            }
            xyzw[i][0] = p[0] - cam->vpn[0] * nudge;
            xyzw[i][1] = p[1] - cam->vpn[1] * nudge;
            xyzw[i][2] = p[2] - cam->vpn[2] * nudge;
            xyzw[i][3] = 1.0f;
            // KIWI-UX (ROUND AL, ITEM 1): a CONSTANT world normal (kiwi_lines.h
            // TRAP 4).  The NUDGE above still reads vpn — that one is about
            // depth and is untouched.
            KiwiTris_FillNormal( nrm[i] );
            st[i][0]   = 0.0f;
            st[i][1]   = 0.0f;
            col[i]     = packedAsFloat;
        }
        // The fan, as an explicit index list: the last wedge closes onto vertex 1.
        for ( int i = 0; i < KSPLIT_DISC_SEGS; ++i )
        {
            idx[i * 3 + 0] = 0;
            idx[i * 3 + 1] = (uint16_t)( 1 + i );
            idx[i * 3 + 2] = (uint16_t)( 1 + ( ( i + 1 ) % KSPLIT_DISC_SEGS ) );
        }

        R_AddCmdSetMaterialColor( s_neutral );
        R_AddRenderCmdDrawTris( g_qeglobals.d_white, TECHNIQUE_UNLIT,
                                (short)( KSPLIT_DISC_SEGS * 3 ), idx,
                                (short)( KSPLIT_DISC_SEGS + 1 ), xyzw, nrm, col, st );
        R_AddCmdSetMaterialColor( s_white );
    }

    void DrawQuad( const float a[3], const float b[3], const float c[3], const float d[3],
                   const float rgba[4] )
    {
        static const float s_neutral[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        static const float s_white[4]   = { 1.0f, 1.0f, 1.0f, 1.0f };

        float          xyzw[4][4];
        float          nrm [4][3];
        float          st  [4][2];
        float          col [4];
        // KIWI-UX (ROUND AA, ITEM 2): was `static const`.  The quad's corners come
        // from m_dir (the picked line) and m_away, and m_away is derived from the
        // camera AS IT WAS AT THE CLICK and then frozen — so the face normal is
        // fixed at click time and orbiting past it made the whole sweep preview
        // disappear.  Same defect round Y fixed on the cut disc.  kiwi_lines.h
        // TRAP 3.  Per-instance now, because KiwiTris_OrientToEye rewrites it.
        uint16_t idx[6] = { 0, 1, 2, 0, 2, 3 };

        float rgbaCopy[4] = { rgba[0], rgba[1], rgba[2], rgba[3] };
        GfxColor packed;
        Byte4PackPixelColor( rgbaCopy, &packed );
        const float packedAsFloat = *(float *)&packed.packed;   // bit-cast, as the ported batcher does

        const float *pts[4] = { a, b, c, d };
        const camera_s *cam = Ed_Camera();
        for ( int i = 0; i < 4; ++i )
        {
            xyzw[i][0] = pts[i][0];
            xyzw[i][1] = pts[i][1];
            xyzw[i][2] = pts[i][2];
            xyzw[i][3] = 1.0f;
            KiwiTris_FillNormal( nrm[i] );   // ROUND AL, ITEM 1 — kiwi_lines.h TRAP 4
            st[i][0]   = 0.0f;
            st[i][1]   = 0.0f;
            col[i]     = packedAsFloat;
        }

        KiwiTris_OrientToEye( &xyzw[0][0], 4, idx, 6, cam->origin );

        R_AddCmdSetMaterialColor( s_neutral );
        R_AddRenderCmdDrawTris( g_qeglobals.d_white, TECHNIQUE_UNLIT,
                                6, idx, 4, xyzw, nrm, col, st );
        R_AddCmdSetMaterialColor( s_white );
    }

    // The polygon a plane carves out of one brush, drawn as the segment it leaves
    // across every face it crosses.  Cheap and exact: a convex face winding meets a
    // plane in at most one segment, and the union of those segments IS the cut
    // outline on the brush's surface.
    void DrawIntersection( const brush_t *def, const float n[3], float d )
    {
        if ( !def || !def->faces )
            return;
        for ( int f = 0; f < def->faceCount; ++f )
        {
            const winding_t *w = def->faces[f].w;
            if ( !w || w->numpoints < 3 || w->numpoints > MAX_POINTS_ON_WINDING )
                continue;

            float hit[2][3];
            int   nhit = 0;
            for ( int i = 0; i < w->numpoints && nhit < 2; ++i )
            {
                const int j = ( i + 1 ) % w->numpoints;
                const float di = Dot3( n, w->p[i] ) - d;
                const float dj = Dot3( n, w->p[j] ) - d;
                if ( ( di > 0.0f ) == ( dj > 0.0f ) )
                    continue;                       // both ends on one side
                const float t = di / ( di - dj );
                for ( int k = 0; k < 3; ++k )
                    hit[nhit][k] = w->p[i][k] + ( w->p[j][k] - w->p[i][k] ) * t;
                ++nhit;
            }
            if ( nhit == 2 )
                KiwiLines_Add( hit[0], hit[1] );
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  The shared splitter (kiwi_split.h).
// ═════════════════════════════════════════════════════════════════════════════
bool KiwiSplit_PlaneCrossesBrush( const brush_t *def,
                                  const float p0[3], const float p1[3], const float p2[3] )
{
    if ( !def )
        return false;
    float n[3], d;
    if ( !PlaneOf( p0, p1, p2, n, &d ) )
        return false;

    // The bounding box's 8 corners: the plane crosses the brush only if they land
    // on both sides.  Conservative in the right direction — a box that straddles
    // while the brush itself does not simply produces a NULL half below, which is
    // already a handled refusal.
    bool front = false, back = false;
    for ( int i = 0; i < 8; ++i )
    {
        const float p[3] = { ( i & 1 ) ? def->maxs[0] : def->mins[0],
                             ( i & 2 ) ? def->maxs[1] : def->mins[1],
                             ( i & 4 ) ? def->maxs[2] : def->mins[2] };
        const float s = Dot3( n, p ) - d;
        if ( s >  KSPLIT_EPS ) front = true;
        if ( s < -KSPLIT_EPS ) back  = true;
    }
    return front && back;
}

// ── ROUND L: the DEF-LEVEL splitter, hoisted out of the instance-level one ──
// kiwi_boolean.cpp needs to split a brush N TIMES IN A ROW, keeping one half and
// re-splitting the other, and only the final pieces may ever reach the map.  That
// is the same three operations this function has always performed (build the
// template face, run the ported core, gate both halves) MINUS the landing, so the
// landing is what moved out rather than the logic being copied.
//
// Contract, and it is deliberately all-or-nothing so a caller's cleanup is one
// line: on `true`, at most ONE of the two halves is NULL, and a NULL one means
// the plane did not divide this brush — everything is on the other side.  On
// `false` NOTHING is allocated, whatever the reason.
//
// WHICH HALF IS WHICH, read out of the ported core rather than assumed
// (brush.cpp:4605 Brush_SplitBrushByFace): `*back` is the clone that gains the
// template face AS GIVEN and `*front` the clone that gains it REVERSED (planepts
// [0] and [1] swapped, which flips the plane).  A brush's interior is the
// intersection of its faces' `n·p <= d` half-spaces, so with `n` the template's
// own outward normal:
//     back  = { n·p <= d }   (BEHIND the plane, the side the normal points away from)
//     front = { n·p >= d }   (IN FRONT of it)
// The subtract in kiwi_boolean.cpp depends on exactly that reading.
//
// ── KIWI-UX (ROUND AO, ITEM 3): THE BODY MOVED DOWN ONE LEVEL ───────────────
// Everything below now lives in `SplitDefByPlaneBody`, which reports the two
// halves SEPARATELY, and this function is the wrapper that reproduces the
// all-or-nothing contract above EXACTLY — including "on false NOTHING is
// allocated" and including which half's §19 reason wins (front before back,
// which is what the short-circuit `&&` it used to be spelled with produced).
// The cut and split verbs therefore see no change whatsoever.
// KIWI-UX (CLEANUP, A-19): the per-half report is not a published entry point —
// KiwiSplit_DefByPlaneCarve is the one external caller that needs it.
static bool SplitDefByPlaneBody( brush_t *def,
                                 const float p0[3], const float p1[3], const float p2[3],
                                 brush_t **outFront, brush_t **outBack,
                                 kiwiSplitHalf_t *outFrontState,
                                 kiwiSplitHalf_t *outBackState,
                                 bool keepRefusedBack, bool *outBackRefused,
                                 const char **why );

bool KiwiSplit_DefByPlane( brush_t *def,
                           const float p0[3], const float p1[3], const float p2[3],
                           brush_t **outFront, brush_t **outBack, const char **why )
{
    const char *localWhy = "unknown";
    if ( !why )
        why = &localWhy;
    if ( outFront ) *outFront = 0;
    if ( outBack  ) *outBack  = 0;

    brush_t         *front = 0, *back = 0;
    kiwiSplitHalf_t  fs = KSPLIT_HALF_NONE, bs = KSPLIT_HALF_NONE;
    if ( !SplitDefByPlaneBody( def, p0, p1, p2, &front, &back, &fs, &bs,
                               false, 0, why ) )   // KIWI-UX (CLEANUP, A-19)
        return false;

    if ( fs == KSPLIT_HALF_SLIVER || bs == KSPLIT_HALF_SLIVER )
    {
        // `why` already names the first bad half's §19 check.  The half that DID
        // survive is freed here so the caller never sees a partial result.
        if ( front ) KiwiSplit_FreeUnlandedDef( front );
        if ( back  ) KiwiSplit_FreeUnlandedDef( back  );
        return false;
    }

    if ( outFront ) *outFront = front;
    if ( outBack  ) *outBack  = back;
    return true;
}

// ── KIWI-UX (ROUND AR, ITEM 2): the shared body ─────────────────────────────
// `keepRefusedBack` is the ONLY difference between the two published entries —
// see kiwi_split.h (§19 IS A GATE ON WHAT GETS LANDED) for the argument.  The
// FRONT half is gated identically in both, because the front is what gets landed.
static bool SplitDefByPlaneBody( brush_t *def,
                                 const float p0[3], const float p1[3], const float p2[3],
                                 brush_t **outFront, brush_t **outBack,
                                 kiwiSplitHalf_t *outFrontState,
                                 kiwiSplitHalf_t *outBackState,
                                 bool keepRefusedBack, bool *outBackRefused,
                                 const char **why )
{
    const char *localWhy = "unknown";
    if ( !why )
        why = &localWhy;
    if ( outFront )      *outFront      = 0;
    if ( outBack  )      *outBack       = 0;
    if ( outFrontState ) *outFrontState = KSPLIT_HALF_NONE;
    if ( outBackState  ) *outBackState  = KSPLIT_HALF_NONE;
    if ( outBackRefused ) *outBackRefused = false;
    if ( !def )
    {
        *why = "no brush";
        return false;
    }

    // Refuse a degenerate plane BEFORE anything is cloned: Face_MakePlane would
    // normalise a zero cross product and hand both halves a garbage plane.
    float cutN[3], cutD;
    if ( !PlaneOf( p0, p1, p2, cutN, &cutD ) )
    {
        *why = "degenerate cut plane";
        return false;
    }

    // ── KIWI-UX (ROUND T): THE TWO NEW FACES INHERIT, THEY ARE NOT CAULKED ───
    // USER DIRECTIVE, verbatim: "Make it so the texture is just inherited from
    // the parent brush that are being operated on.  The caulk texture is not
    // usable."
    //
    // WAS: Ed_BuildClipFaceMaterial_Kiwi( &clipFace, def ) — the CLIPPER's caulk /
    // nodraw_decal synthesis, which is right for the clipper (it trims structural
    // brushwork and the binary caulks the trim) and wrong for "split this in two",
    // where the halves are meant to look like the thing that was split.  It is
    // also the depth-writeless class round M/O decoded, i.e. the z-order lottery.
    //
    // NOW: kiwi_material.h R2 — the source brush's largest INHERITABLE face whose
    // plane is most perpendicular to this cut.  R3 keeps an all-tool brush a tool
    // brush by falling back to the same synthesis, so a caulk block still cuts
    // into caulk.  The CLASSIC clipper is untouched and still caulks.
    //
    // face_t{} zero-inits, which is what leaves `w` NULL — Face_Alloc clones the
    // template's winding and NULL is exactly what the clipper's own template
    // carries.
    face_t clipFace{};
    KiwiMtl_SeedClipFace( &clipFace, def, cutN );
    for ( int k = 0; k < 3; ++k )
    {
        clipFace.planepts[0][k] = p0[k];
        clipFace.planepts[1][k] = p1[k];
        clipFace.planepts[2][k] = p2[k];
    }

    brush_t *front = 0, *back = 0;
    Brush_SplitBrushByFace( def, &clipFace, &front, &back );

    if ( !front && !back )
    {
        *why = "the plane left nothing on either side";
        return false;
    }

    // §19 on whichever halves exist, while they are still off every display list.
    // Brush_SplitBrushByFace already ran Brush_BuildWindings( b, 1 ) on each, so
    // planes, windings and bounds are current and no rebuild is due here.  A
    // half that fails is FREED HERE — the UNLINK + FREE pair CSG_MakeHollow uses
    // on the piece it throws away (csg.cpp:365-369) — and reported as
    // KSPLIT_HALF_SLIVER, so no caller ever has to unpick a partial result and no
    // caller is handed a def it did not ask about.
    //
    // ROUND AO, ITEM 3: the two halves are gated INDEPENDENTLY.  `why` keeps the
    // FRONT's reason when both are bad, which is the order the single `&&` this
    // replaced short-circuited in.
    const char *frontWhy = "invalid geometry";
    const char *backWhy  = "invalid geometry";
    kiwiSplitHalf_t fs = KSPLIT_HALF_NONE;
    kiwiSplitHalf_t bs = KSPLIT_HALF_NONE;

    if ( front )
    {
        if ( KiwiValid_CheckBrush( front, &frontWhy ) )
        {
            fs = KSPLIT_HALF_OK;
        }
        else
        {
            fs = KSPLIT_HALF_SLIVER;
            Entity_UnlinkBrush( front );
            Brush_Free_R( front );
            front = 0;
        }
    }
    if ( back )
    {
        if ( KiwiValid_CheckBrush( back, &backWhy ) )
        {
            bs = KSPLIT_HALF_OK;
        }
        // ── KIWI-UX (ROUND AR, ITEM 2): CARRY A REFUSED *INTERMEDIATE* ───────
        // The back half of a subtract step is never landed (kiwi_boolean.cpp
        // discards it), so a §19 refusal of it is a statement about presentation
        // and not about volume.  It is carried on when it has real thickness in
        // all three directions; anything thinner is round AO's genuine graze and
        // still stops the cascade.  Bounds are current: Brush_SplitBrushByFace
        // ran Brush_BuildWindings on both halves.
        //
        // THE SECOND HALF OF THE TEST IS A CONTAINMENT INVARIANT, and it is what
        // keeps this from carrying nonsense.  A half-space cut of a solid is a
        // SUBSET of it, so the back's bounds must sit inside the input's.  Any §19
        // verdict that means "this is not a bounded solid at all" — V7 (span past
        // the map bound) and V8 (the remaining half-spaces do not enclose a finite
        // cell, kiwi_validity.h) — breaks that invariant and is refused here, so
        // only the presentation verdicts (V3 / V4 / V5 / V6 on a real subset) can
        // ever be carried.  One world unit of slack for the split's own rounding.
        else if ( keepRefusedBack
               && ( back->maxs[0] - back->mins[0] ) >= KSPLIT_CARRY_EXTENT
               && ( back->maxs[1] - back->mins[1] ) >= KSPLIT_CARRY_EXTENT
               && ( back->maxs[2] - back->mins[2] ) >= KSPLIT_CARRY_EXTENT
               && back->mins[0] >= def->mins[0] - 1.0f && back->maxs[0] <= def->maxs[0] + 1.0f
               && back->mins[1] >= def->mins[1] - 1.0f && back->maxs[1] <= def->maxs[1] + 1.0f
               && back->mins[2] >= def->mins[2] - 1.0f && back->maxs[2] <= def->maxs[2] + 1.0f )
        {
            bs = KSPLIT_HALF_OK;
            if ( outBackRefused )
                *outBackRefused = true;
        }
        else
        {
            bs = KSPLIT_HALF_SLIVER;
            Entity_UnlinkBrush( back );
            Brush_Free_R( back );
            back = 0;
        }
    }

    if ( fs == KSPLIT_HALF_SLIVER )
        *why = frontWhy;
    else if ( bs == KSPLIT_HALF_SLIVER )
        *why = backWhy;
    // KIWI-UX (ROUND AR, ITEM 2): a CARRIED back half still names the gate it
    // failed, so the caller's "the hole may be imperfect here" line can say which
    // one it was instead of only that something happened.
    else if ( outBackRefused && *outBackRefused )
        *why = backWhy;

    if ( outFront )      *outFront      = front;
    if ( outBack  )      *outBack       = back;
    if ( outFrontState ) *outFrontState = fs;
    if ( outBackState  ) *outBackState  = bs;
    return true;
}

// KIWI-UX (ROUND AR, ITEM 2) — see kiwi_split.h for the whole argument.
bool KiwiSplit_DefByPlaneCarve( brush_t *def,
                                const float p0[3], const float p1[3], const float p2[3],
                                brush_t **outFront, brush_t **outBack,
                                kiwiSplitHalf_t *outFrontState,
                                kiwiSplitHalf_t *outBackState,
                                bool *outBackRefused,
                                const char **why )
{
    return SplitDefByPlaneBody( def, p0, p1, p2, outFront, outBack,
                                outFrontState, outBackState,
                                true, outBackRefused, why );
}

void KiwiSplit_FreeUnlandedDef( brush_t *def )
{
    if ( !def )
        return;
    Entity_UnlinkBrush( def );
    Brush_Free_R( def );
}

bool KiwiSplit_BrushByPlane( selbrush_t *node,
                             const float p0[3], const float p1[3], const float p2[3],
                             selbrush_t **outA, selbrush_t **outB, const char **why )
{
    const char *localWhy = "unknown";
    if ( !why )
        why = &localWhy;
    if ( outA ) *outA = 0;
    if ( outB ) *outB = 0;

    if ( !Splittable( node ) )
    {
        *why = "not a splittable brush";
        return false;
    }
    brush_t  *def   = node->def;
    entity_s *owner = node->owner;

    brush_t *front = 0, *back = 0;
    if ( !KiwiSplit_DefByPlane( def, p0, p1, p2, &front, &back, why ) )
        return false;

    // A NULL half means that side kept fewer than 4 faces, i.e. the plane did not
    // actually divide this brush.  THIS entry point wants two halves or nothing,
    // so discard whatever did come back.
    if ( !front || !back )
    {
        KiwiSplit_FreeUnlandedDef( front );
        KiwiSplit_FreeUnlandedDef( back );
        *why = "the plane does not cross this brush";
        return false;
    }

    // Land both, SELECTED, in the ported order (Ed_ProduceSplitLists and
    // CSG_MakeHollow both do exactly this pair, with the same already-linked
    // guard).  The source instance is NOT freed here — kiwi_split.h says why.
    selbrush_t *na = Brush_AddToList( front, owner );
    if ( na->next || na->prev )
        Com_Error( ERR_FATAL, "Brush_AddToList: already linked" );
    Brush_AddToList2( na );

    selbrush_t *nb = Brush_AddToList( back, owner );
    if ( nb->next || nb->prev )
        Com_Error( ERR_FATAL, "Brush_AddToList: already linked" );
    Brush_AddToList2( nb );

    if ( outA ) *outA = na;
    if ( outB ) *outB = nb;
    g_nUpdateBits = -1;
    return true;
}

namespace
{
    // Split every brush on `targets` and free each source.  Returns how many
    // brushes were actually divided.  The CALLER owns the undo bracket: this must
    // run between KiwiCmd_UndoBegin and KiwiCmd_UndoCommit (kiwi_split.h).
    int SplitTargets( const std::vector<selbrush_t *> &targets,
                      const float p0[3], const float p1[3], const float p2[3],
                      const char *verb )
    {
        int done = 0;
        for ( size_t i = 0; i < targets.size(); ++i )
        {
            selbrush_t *node = targets[i];
            if ( !Sel_BrushLive( node ) )
                continue;                        // freed under us between passes

            const char *why = "unknown";
            selbrush_t *a = 0, *b = 0;
            if ( !KiwiSplit_BrushByPlane( node, p0, p1, p2, &a, &b, &why ) )
            {
                Sys_Printf( "%s: brush skipped — %s.\n", verb, why );
                continue;
            }
            // Both halves are linked and selected; only NOW is the source freed,
            // so its owner entity never transiently runs out of brushes.
            Brush_Free( node );
            ++done;
        }
        return done;
    }

    // ═════════════════════════════════════════════════════════════════════════
    //  C — CUT.  Selected brushes, a CLICKED construction line, and a plane that
    //  is locked at the instant of that click.  See kiwi_split.h THE CUT PLANE for
    //  the derivation and ROUND L: THE TWO-STAGE CUT for the flow.
    // ═════════════════════════════════════════════════════════════════════════
    class KiwiCutCommand : public KiwiEditorCommand
    {
    public:
        const char *Name() const override { return "Cut"; }
        bool CanExecute() override { return KiwiSplit_CanCut(); }

        // ── ROUND L: STAGE 1 IS A CLICK TOOL, STAGE 2 IS NOT ─────────────────
        // Stage 1 needs the drawing tools' grammar (a click NAMES something — the
        // same shape Match Face uses to pick its target face), and stage 2 needs
        // the shakeout-E confirm flow (RMB / Enter commits, a stray click parks).
        // WantsClicks is asked per press, so one command can be both in turn.
        // ── KIWI-UX (ROUND AF, ITEM 1): STAGE 2 IS A CLICK TOOL TOO ────────
        // USER DIRECTIVE, verbatim: "Allow shift-clicking of lines during cut
        // operation setup to enable this."
        //
        // A SECOND line can only be named by a SECOND CLICK, and the FIRST click
        // is what enters stage 2 — so with stage 2 refusing clicks there was no
        // reachable state in which a fence could be grown at all.  The clicks
        // therefore never stop being clicks, exactly as round AA did for the
        // boolean when its tool became a set, and with the same cost accounted for:
        // what is given up is the park-on-click behaviour (KiwiCmd_Pause returns
        // early for a click tool), and the cut has NO DRAG to park — the plane is
        // locked at the click and nothing follows the cursor except the hover,
        // which is exactly what has to keep working so the next line can be aimed
        // at.  RMB / Enter still commits and Esc still walks the stages back;
        // neither goes through this rung.
        bool WantsClicks() const override { return true; }

        // KIWI-UX (CLEANUP, A-40): no numeric field -- the plane comes from the
        // picked source.  There is no NumericFields override here at all (the
        // earlier note claimed one was "declared"), and leaving Tab free is
        // deliberate: kiwi_split.h TAB ROUTING.
        const char *HudStatus() const override { return m_hud[0] ? m_hud : 0; }
        // RED while the LOCKED plane crosses NOTHING: confirming then would
        // silently do nothing, and the HUD is the only place that can say so in
        // advance.  Never red in stage 1 — there is no plane yet to be wrong.
        bool        HudInvalid() const override
        { return m_stage == KCUT_PREVIEW && m_crossing <= 0; }

        // ROUND AF, ITEM 3's hook, used by ROUND AF, ITEM 1.  A 64-plane fence is
        // 256 outline segments plus the per-target intersection outlines, which is
        // several times the framework's default batch — the same "the cost is a
        // function of a set the user is still growing" shape the boolean has, and
        // answered the same way.  0 (no fence) leaves the default untouched.
        int LineBudget() const override
        {
            if ( !FenceActive() )
                return 0;
            return FenceCount() * 4 + (int)m_targets.size() * FenceCount() * 8 + 96;
        }

        // The cut must never pick or snap to the geometry it is about to divide.
        unsigned PickFlags() const override { return PICKF_EXCLUDE_SELECTED; }

        int HudPrompts( const kiwiPrompt_t **out ) const override
        {
            // STATIC storage per kiwi_command.h's contract — the strip copies the
            // structs, not the strings.  The framework's own keys (confirm /
            // cancel) are derived by kiwi_hints.cpp and not repeated.
            // ROUND AF, ITEM 1: the additive grammar is advertised in BOTH stages,
            // because it IS the same grammar in both — a fence can be grown after the
            // preview is live, exactly as the boolean's tool set can.
            static const kiwiPrompt_t s_pick[] = {
                { "LMB",       "Pick a line, a brush EDGE or a brush FACE" },
                { "Shift+LMB", "Add a line" },
            };
            static const kiwiPrompt_t s_preview[] = {
                { "Shift+LMB", "Add a line" },
                { "Esc",       "Pick another plane" },
            };
            if ( m_stage == KCUT_PICK_LINE )
            {
                *out = s_pick;
                return (int)( sizeof( s_pick ) / sizeof( s_pick[0] ) );
            }
            *out = s_preview;
            return (int)( sizeof( s_preview ) / sizeof( s_preview[0] ) );
        }

        bool Begin() override
        {
            Reset();

            for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
                if ( Splittable( b ) )
                    m_targets.push_back( b );
            if ( m_targets.empty() )
            {
                Sys_Printf( "Cut: select at least one brush (patches and fixed-size "
                            "entities cannot be cut).\n" );
                return false;
            }

            // ── KIWI-UX (ROUND T): NOTHING REFUSES THE GESTURE ANY MORE ─────
            // Round L kept ONE precondition — "there has to be a construction line
            // somewhere" — on the honest ground that "click a line" is unanswerable
            // from inside a modal gesture when the map has none.  It is answerable
            // now: a brush face or a brush edge names a plane just as well, and
            // every map has those.  The test survives only to word the prompt.
            m_stage = KCUT_PICK_LINE;
            UpdateHud();
            Sys_Printf( "Cut: click %s, a brush EDGE, or a brush FACE to use its "
                        "plane (Esc cancels).\n",
                        AnyConstructionSegment() ? "a construction line"
                                                 : "a construction line (there are none)" );
            return true;
        }

        // ── ROUND L: STAGE 1 HOVERS, STAGE 2 DOES NOTHING AT ALL ─────────────
        // USER DIRECTIVE, verbatim: "Also the way it cuts needs to be decided at
        // line click-time and not update as the camera moves."
        //
        // BEFORE (shakeout G): AimFromCamera() ran HERE, on every hot frame, so the
        // cutting plane swung with the camera for the whole life of the gesture and
        // only stopped when the user happened to park it.  A cut you had aimed
        // could be re-aimed by an orbit meant only to look at the preview.
        //
        // AFTER: the plane is derived ONCE, inside Click(), from the clicked line
        // and the camera's vpn AT THAT INSTANT, and stored.  Nothing re-derives it —
        // there is no call to AimFromCamera anywhere on a per-frame path.  Orbiting
        // in stage 2 moves the camera and nothing else, which is what "decided at
        // line click-time" means.
        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick;
            // ── ROUND AF, ITEM 1: THE HOVER TRACKS IN BOTH STAGES ──────────
            // It used to return here ("the plane is LOCKED: nothing to update"),
            // which was true when there was only ever one source.  With a FENCE
            // there is always a next line to aim at, so the hover has to stay alive
            // for the whole gesture — it is what tells the user which shape the
            // next Shift+click will take, and it is what Click() reads.
            //
            // NOTHING ABOUT THE LOCKED PLANE MOVES.  m_p, m_planeN/D, m_lineA/B and
            // m_away are written only by AimFromCamera / AimFromFace, both of which
            // are called only from Click().  Round L's "the plane locks at the
            // click and never re-derives" rule is untouched; only the QUESTION
            // "what is under the cursor" keeps being asked.
            const bool preview = ( m_stage != KCUT_PICK_LINE );

            // ROUND X, ITEM 7: the point the disc is drawn at.  The SNAP position,
            // not the raw pick, because that IS where the click will cut and it is
            // the point the dot-and-ring marker is already sitting on — two markers
            // for one act must not be able to disagree about where the act is.
            // ROUND AF, ITEM 1: the DISC is a stage-1 affordance ("the click will cut
            // here") and would be noise over a locked preview, so it is the one thing
            // that stays stage-1 only.
            m_haveDisc = !preview && snap.valid;
            if ( m_haveDisc )
                Copy3( snap.position, m_discPt );

            int   x, y;
            const bool      was     = m_haveHover;
            const cutPick_t wasWhat = m_hover;
            m_haveHover = false;
            if ( KiwiCmd_LastCursor( &x, &y ) )
                m_haveHover = PickCutSourceAt( x, y, &m_hover );   // ROUND T: 3 kinds
            // The disc repaints with the cursor, not only when the hovered THING
            // changes: it follows the snap point across a single face.
            g_nUpdateBits |= 1;
            // Repaint when the hover APPEARS, DISAPPEARS or moves to a different
            // thing — a face hover is an area hit, so "the flag changed" alone
            // would leave the highlight stuck on the previous face while the
            // cursor slid across a wall.
            if ( m_haveHover != was
              || ( m_haveHover && ( m_hover.kind      != wasWhat.kind
                                 || m_hover.object    != wasWhat.object
                                 || m_hover.segment   != wasWhat.segment
                                 || m_hover.node      != wasWhat.node
                                 || m_hover.faceIndex != wasWhat.faceIndex ) ) )
                g_nUpdateBits |= 1;
            UpdateHud();
        }

        // Stage 1's click LOCKS THE PLANE.  Return true keeps the gesture running
        // (kiwi_command.h Click) — this command is never committed by a click, only
        // by RMB / Enter in stage 2.
        bool Click() override
        {
            if ( m_stage != KCUT_PICK_LINE )
                return true;
            if ( !m_haveHover )
            {
                Sys_Printf( "Cut: nothing to cut along there — click a construction "
                            "line, a brush edge, or a brush face, or press Esc.\n" );
                return true;
            }

            m_srcKind = m_hover.kind;
            // ROUND AF, ITEM 1: an EDGE or a FACE is a SINGLE-plane source and has
            // no object to fence with, so picking one drops any fence outright
            // rather than leaving a set that FenceActive would still answer true
            // for while m_p holds a different plane entirely.
            if ( m_hover.kind != KCUT_SRC_LINE )
            {
                m_fenceObjs.clear();
                m_fence.clear();
            }

            if ( m_srcKind == KCUT_SRC_FACE )
            {
                // ── ROUND T: THE FACE ARM.  NO CAMERA TERM AT ALL ────────────
                // A face already IS a plane, so MeasureBounds (which the camera
                // arm needs for its lever arm) is here only to size the preview
                // and to place the three planepts near the geometry being cut —
                // near, because Face_MakePlane's normal comes from a cross
                // product of the planept differences and points a thousand units
                // away from the action are needless precision loss.
                MeasureBounds();
                if ( !AimFromFace() )
                {
                    Sys_Printf( "Cut: that face has a degenerate plane — pick "
                                "another.\n" );
                    return true;
                }
                m_stage = KCUT_PREVIEW;
                CountCrossings();
                UpdateHud();
                Sys_Printf( "Cut: cutting with that face's plane — RMB / Enter cuts, "
                            "Esc picks another.\n" );
                g_nUpdateBits = -1;
                return true;
            }

            // ══════════════════════════════════════════════════════════════
            //  KIWI-UX (ROUND AF, ITEM 1): A CONSTRUCTION LINE NAMES A FENCE
            // ══════════════════════════════════════════════════════════════
            // USER DIRECTIVE, verbatim: "When cutting, a lot of times I want to
            // cut with an entire circle or half-circle.  Allow shift-clicking of
            // lines during cut operation setup to enable this."
            //
            // A CLICK NAMES THE WHOLE OBJECT, not the segment under the cursor.
            // For a straight line that is exactly what it always was; for a circle
            // or a polyline it is the loop the directive asks for, and it needs no
            // new picking — KiwiCon_SegmentCount / SegmentWorld have always been
            // able to walk an object.  SHIFT adds another object; shift-clicking
            // one already in takes it back out.
            //
            // The BRUSH-EDGE arm is untouched and falls through to the round-L
            // single-plane derivation below: an edge is not a construction object,
            // it has no plane of its own to sweep along, and nothing in the
            // directive asks for a fence of them.
            if ( m_srcKind == KCUT_SRC_LINE && m_hover.object >= 0 )
            {
                const bool add = KiwiCmd_LastShift();
                if ( !add )
                    m_fenceObjs.clear();
                bool removed = false;
                for ( size_t i = 0; i < m_fenceObjs.size(); ++i )
                    if ( m_fenceObjs[i] == m_hover.object )
                    {
                        m_fenceObjs.erase( m_fenceObjs.begin() + i );
                        removed = true;
                        break;
                    }
                if ( !removed )
                    m_fenceObjs.push_back( m_hover.object );

                if ( m_fenceObjs.empty() )
                {
                    m_fence.clear();
                    m_stage = KCUT_PICK_LINE;
                    UpdateHud();
                    Sys_Printf( "Cut: nothing selected to cut with — click a line, "
                                "a circle, a brush edge or a brush face.\n" );
                    g_nUpdateBits = -1;
                    return true;
                }

                MeasureBounds();
                if ( !FenceRebuild() )
                {
                    m_fenceObjs.clear();
                    m_fence.clear();
                    Sys_Printf( "Cut: that geometry gives no sweep direction — it "
                                "points at the camera and carries no plane.  Orbit, "
                                "then click it again.\n" );
                    return true;                 // stay in stage 1
                }

                if ( FenceActive() )
                {
                    // A REAL FENCE.  m_p is left alone deliberately: it is the
                    // SINGLE-plane path's state and the fence path never reads it,
                    // so the two cannot half-mix.
                    m_stage = KCUT_PREVIEW;
                    CountCrossings();
                    UpdateHud();
                    Sys_Printf( "Cut: %i segment(s) from %i object(s), swept along "
                                "%s — every piece is KEPT (a closed loop separates "
                                "inside from outside).  Shift+click adds more, "
                                "RMB / Enter cuts.\n",
                                FenceCount(), (int)m_fenceObjs.size(),
                                m_fenceFromPlane ? "its own plane normal"
                                                 : "the view direction" );
                    g_nUpdateBits = -1;
                    return true;
                }
                // Exactly one segment: fall through to the round-L single-plane
                // path, byte for byte, using the segment the fence flattened to.
                Copy3( &m_fence[0], m_hover.a );
                Copy3( &m_fence[3], m_hover.b );
            }

            // LINE and EDGE share the derivation completely: two endpoints plus the
            // camera.  An edge IS a line here, which is why the arm is not two arms.
            Copy3( m_hover.a, m_lineA );
            Copy3( m_hover.b, m_lineB );
            Sub3( m_lineB, m_lineA, m_dir );
            if ( !Norm3( m_dir ) )
            {
                Sys_Printf( "Cut: that segment has zero length — pick another.\n" );
                return true;
            }

            // MeasureBounds first (AimFromCamera's third planept uses the lever arm
            // it computes), then the ONE and ONLY derivation of the plane.
            MeasureBounds();
            if ( !AimFromCamera() )
            {
                Sys_Printf( "Cut: that %s points at the camera — there is no sweep "
                            "direction.  Orbit, then click it again.\n",
                            ( m_srcKind == KCUT_SRC_EDGE ) ? "edge" : "line" );
                return true;                     // stay in stage 1, plane not locked
            }

            m_stage = KCUT_PREVIEW;
            CountCrossings();
            UpdateHud();
            Sys_Printf( "Cut: plane locked from this view — RMB / Enter cuts, Esc "
                        "picks another.\n" );
            g_nUpdateBits = -1;
            return true;
        }

        // Esc walks the stages back before the framework's cancel rung ever sees it
        // (kiwi_command.cpp's ladder offers the key to the command first); Enter in
        // stage 1 is consumed with a nudge, because there is nothing to confirm yet
        // and committing an unlocked plane would silently do nothing.
        bool KeyDown( int vk, unsigned mods ) override
        {
            (void)mods;
            if ( vk == 0x1B && m_stage == KCUT_PREVIEW )     // VK_ESCAPE
            {
                m_stage     = KCUT_PICK_LINE;
                m_crossing  = 0;
                m_haveHover = false;
                // A stage-2 click can have PARKED the gesture (stage 2 is not a
                // click tool, so shakeout E's pause arm applies to it), and a
                // PAUSED command receives no MouseMove — the line hover would be
                // dead and the next click would be eaten as a resume.  Stage 1 can
                // never be paused (WantsClicks refuses), so this is a no-op except
                // on exactly the edge that needs it.
                KiwiCmd_Resume();
                UpdateHud();
                Sys_Printf( "Cut: plane dropped — click another line, edge or face "
                            "(Esc again cancels).\n" );
                g_nUpdateBits = -1;
                return true;
            }
            if ( vk == 0x0D && m_stage == KCUT_PICK_LINE )   // VK_RETURN
            {
                Sys_Printf( "Cut: click a line to cut along first.\n" );
                return true;
            }
            return false;
        }

        void DrawWorld() override
        {
            if ( m_targets.empty() )
                return;

            if ( m_stage == KCUT_PICK_LINE )
            {
                // ── ROUND X, ITEM 7: the translucent cut disc ────────────────
                // Drawn FIRST so the hover highlight and the snap marker land on
                // top of it, which is what makes it read as an area under a point
                // rather than as a blob over one.  Screen-constant at 15 px (2.5x
                // the snap ring), oriented on the surface being cut when there is
                // one — a FACE hover has a plane; a line or an edge does not, and
                // DrawDisc faces the camera for those.
                if ( m_haveDisc )
                {
                    const float r = KiwiSnap_RingPixels() * KSPLIT_DISC_SCALE
                                  * KiwiCam_WorldPerPixel( m_discPt );
                    const float *onSurface = 0;
                    if ( m_haveHover && m_hover.kind == KCUT_SRC_FACE )
                        onSurface = m_hover.n;
                    DrawDisc( m_discPt, onSurface, r, KSPLIT_DISC_RGBA );
                }

                // The hovered segment, brightened over the construction store's own
                // draw.  Nothing else: with no plane locked there is nothing to
                // preview, and a sweep quad that followed the cursor would be
                // exactly the "updates as the camera moves" the directive removed.
                if ( m_haveHover )
                {
                    KiwiLines_Color( KSPLIT_HOT_COL[0], KSPLIT_HOT_COL[1], KSPLIT_HOT_COL[2] );
                    // ROUND T: whichever of the three kinds would be taken, drawn as
                    // itself — the segment for a line or an edge, the whole winding
                    // for a face.  "Hover-highlight whichever would be picked" is the
                    // directive, and a face highlighted as a two-point stub would not
                    // read as a face.
                    if ( m_hover.kind == KCUT_SRC_FACE )
                        DrawFaceOutline( m_hover );
                    else
                        KiwiLines_Add( m_hover.a, m_hover.b );
                }
                return;
            }

            // ── KIWI-UX (ROUND AF, ITEM 1): THE FENCE PREVIEW ───────────────
            // One swept quad OUTLINE per plane — the segment, its two rails and the
            // far edge — plus the hover, so the next Shift+click is aimable.  Lines
            // rather than the translucent DrawQuad the single plane gets: a
            // sixty-plane tube of translucent fills is an opaque blob that hides the
            // geometry the user is trying to aim at, and the outline reads as a
            // cookie cutter at every count.
            if ( FenceActive() )
            {
                const float lift = m_radius;   // KIWI-UX (CLEANUP, A-39)
                KiwiLines_Color( KSPLIT_EDGE_COL[0], KSPLIT_EDGE_COL[1], KSPLIT_EDGE_COL[2] );
                for ( int f = 0; f < FenceCount(); ++f )
                {
                    const float *sa = &m_fence[(size_t)f * 6];
                    const float *sb = &m_fence[(size_t)f * 6 + 3];
                    float ta[3], tb[3], ba[3], bb[3];
                    Mad3( sa, m_fenceN,  lift, ta );
                    Mad3( sb, m_fenceN,  lift, tb );
                    Mad3( sa, m_fenceN, -lift, ba );
                    Mad3( sb, m_fenceN, -lift, bb );
                    if ( !KiwiLines_Add( ba, bb ) ) break;
                    if ( !KiwiLines_Add( ta, tb ) ) break;
                    if ( !KiwiLines_Add( ba, ta ) ) break;
                    if ( !KiwiLines_Add( bb, tb ) ) break;
                }
                KiwiLines_Color( KSPLIT_LINE_COL[0], KSPLIT_LINE_COL[1], KSPLIT_LINE_COL[2] );
                for ( size_t i = 0; i < m_targets.size(); ++i )
                    if ( Sel_BrushLive( m_targets[i] ) )
                        for ( int f = 0; f < FenceCount(); ++f )
                        {
                            float pts[3][3], n[3];
                            if ( !FencePlanePts( f, pts ) )
                                continue;
                            float e0[3], e1[3];
                            Sub3( pts[1], pts[0], e0 );
                            Sub3( pts[2], pts[0], e1 );
                            Cross3( e0, e1, n );
                            if ( !Norm3( n ) )
                                continue;
                            DrawIntersection( m_targets[i]->def, n, Dot3( n, pts[0] ) );
                        }
                if ( m_haveHover && m_hover.kind == KCUT_SRC_LINE
                  && !FenceHasObject( m_hover.object ) )
                {
                    KiwiLines_Color( KSPLIT_HOT_COL[0], KSPLIT_HOT_COL[1], KSPLIT_HOT_COL[2] );
                    KiwiLines_Add( m_hover.a, m_hover.b );
                }
                return;
            }

            // The sweep quad.  For a LINE / EDGE plane: from the line, straight away
            // from the camera AS IT WAS AT THE CLICK, sized off the affected brushes'
            // bounds (kiwi_split.h).  For a FACE plane there is no sweep direction and
            // no line — the quad is simply a square patch OF the plane, centred on the
            // targets, so what is drawn is what will cut.
            float a[3], b[3], c[3], d[3];
            const float span  = m_radius * KSPLIT_SPAN_SCALE;
            const bool  faceMode = ( m_srcKind == KCUT_SRC_FACE );
            const float depth = faceMode ? ( span * 2.0f )
                                         : ( m_radius * KSPLIT_DEPTH_SCALE );
            float base[3];
            if ( faceMode ) Mad3( m_centre, m_away, -span, base );
            else            Copy3( m_centre, base );
            Mad3( base, m_dir, -span, a );
            Mad3( base, m_dir,  span, b );
            Mad3( b, m_away, depth, c );
            Mad3( a, m_away, depth, d );
            DrawQuad( a, b, c, d, KSPLIT_PLANE_RGBA );

            // The cut LINE itself, then the outline the plane leaves on every
            // affected brush.  Both inside the framework's own batch.  A FACE-derived
            // plane has no line to draw — drawing m_lineA/m_lineB there would draw a
            // stale segment from a previous stage-1 hover.
            if ( !faceMode )
            {
                KiwiLines_Color( KSPLIT_EDGE_COL[0], KSPLIT_EDGE_COL[1], KSPLIT_EDGE_COL[2] );
                KiwiLines_Add( m_lineA, m_lineB );
            }

            KiwiLines_Color( KSPLIT_LINE_COL[0], KSPLIT_LINE_COL[1], KSPLIT_LINE_COL[2] );
            for ( size_t i = 0; i < m_targets.size(); ++i )
                if ( Sel_BrushLive( m_targets[i] ) )
                    DrawIntersection( m_targets[i]->def, m_planeN, m_planeD );
        }

        void Commit() override
        {
            if ( m_stage != KCUT_PREVIEW )
            {
                Sys_Printf( "Cut: no cutting plane was picked — nothing was cut.\n" );
                Reset();
                return;
            }

            // Only the brushes the LOCKED plane actually crosses.  Re-tested at
            // COMMIT rather than reused from the preview count because the BRUSHES
            // can change under a paused gesture (something freed, something moved),
            // not because the plane can — since ROUND L it cannot.
            std::vector<selbrush_t *> hit;
            for ( size_t i = 0; i < m_targets.size(); ++i )
            {
                selbrush_t *node = m_targets[i];
                if ( !Sel_BrushLive( node ) )
                    continue;
                if ( FenceActive() )
                {
                    // ROUND AF, ITEM 1: ANY plane of the fence crossing the brush is
                    // reason enough to hand it to the cascade, which decides per plane
                    // and leaves a target every plane misses completely alone.
                    for ( int f = 0; f < FenceCount(); ++f )
                    {
                        float pts[3][3];
                        if ( !FencePlanePts( f, pts ) )
                            continue;
                        if ( KiwiSplit_PlaneCrossesBrush( node->def, pts[0], pts[1], pts[2] ) )
                        {
                            hit.push_back( node );
                            break;
                        }
                    }
                    continue;
                }
                if ( KiwiSplit_PlaneCrossesBrush( node->def, m_p[0], m_p[1], m_p[2] ) )
                    hit.push_back( node );
            }
            if ( hit.empty() )
            {
                Sys_Printf( "Cut: the plane crosses none of the selected brushes — "
                            "nothing was cut.\n" );
                Reset();
                return;
            }

            // ONE record for "remove N, add 2N" — the CSG_MakeHollow wrapper's
            // exact order (kiwi_split.h UNDO).  KiwiCmd_UndoBegin IS
            // ClearRedo + GeneralStart + AddBrushList(&selected_brushes), which
            // clones every original before the first of them is touched.
            KiwiCmd_UndoBegin( "cut brushes" );
            if ( FenceActive() )
            {
                // ROUND AF, ITEM 1: the cascade.  ONE bracket for the whole fence,
                // exactly as the single-plane path has one for its single split, and
                // for the same reason — this is one gesture.
                int pieces = 0;
                const int cut = CascadeCut( hit, &pieces );
                Sys_Printf( "Cut: %i brush(es) -> %i, by a %i-plane fence.\n",
                            cut, pieces, FenceCount() );
            }
            else
            {
                const int n = SplitTargets( hit, m_p[0], m_p[1], m_p[2], "Cut" );
                Sys_Printf( "Cut: %i brush(es) -> %i.\n", n, n * 2 );
            }

            Reset();
            g_nUpdateBits = -1;
        }

        void Cancel() override
        {
            Reset();
            g_nUpdateBits |= 1;
        }

    private:
        void Reset()
        {
            m_targets.clear();
            m_stage     = KCUT_PICK_LINE;
            m_srcKind   = KCUT_SRC_LINE;
            m_crossing  = 0;
            m_haveHover = false;
            m_haveDisc  = false;      // ROUND X, ITEM 7
            m_fenceObjs.clear();      // ROUND AF, ITEM 1
            m_fence.clear();
            m_fenceFromPlane = false;
            m_hud[0]    = '\0';
        }

        // ── ROUND T: the hovered FACE's winding, as a closed outline ─────────
        // Read live rather than cached: the pick is a frame old by the time this
        // runs and the brush can have been rebuilt under a paused gesture, so the
        // winding pointer is re-fetched and re-bounds-checked here.
        static void DrawFaceOutline( const cutPick_t &h )
        {
            if ( !Sel_BrushLive( h.node ) || !h.node->def || !h.node->def->faces )
                return;
            if ( h.faceIndex < 0 || h.faceIndex >= h.node->def->faceCount )
                return;
            const winding_t *w = h.node->def->faces[h.faceIndex].w;
            if ( !w || w->numpoints < 3 || w->numpoints > MAX_POINTS_ON_WINDING )
                return;
            for ( int i = 0; i < w->numpoints; ++i )
                if ( !KiwiLines_Add( w->p[i], w->p[( i + 1 ) % w->numpoints] ) )
                    return;
        }

        // ── ROUND T: THE FACE ARM'S DERIVATION ───────────────────────────────
        // The clicked face's plane, VERBATIM: m_planeN / m_planeD are its own, and
        // the three planepts are laid out around the point of that plane nearest
        // the targets' centre, in an orthonormal in-plane basis at the bounds
        // radius.  Two consequences worth stating:
        //   * the camera is not consulted, so orbiting between the click and the
        //     confirm cannot change the cut — the ROUND L promise, for free;
        //   * the planepts are WELL SPREAD and NEAR THE GEOMETRY, which is what
        //     Face_MakePlane's cross product wants.  Taking three of the source
        //     winding's own points would satisfy neither on a small face far away.
        //
        // The basis doubles as the preview quad's axes (m_dir / m_away), which is
        // why they are written here rather than only inside DrawWorld.
        bool AimFromFace()
        {
            float n[3];
            Copy3( m_hover.n, n );
            if ( !Norm3( n ) )
                return false;

            // An in-plane basis: cross the normal with whichever world axis it is
            // least aligned to, which can never be the degenerate pairing.
            float ax[3] = { 0.0f, 0.0f, 0.0f };
            {
                int   least = 0;
                float best  = fabsf( n[0] );
                for ( int k = 1; k < 3; ++k )
                    if ( fabsf( n[k] ) < best ) { best = fabsf( n[k] ); least = k; }
                ax[least] = 1.0f;
            }
            float e1[3], e2[3];
            Cross3( n, ax, e1 );
            if ( !Norm3( e1 ) )
                return false;
            Cross3( n, e1, e2 );
            if ( !Norm3( e2 ) )
                return false;

            // The targets' bounds centre (MeasureBounds' m_mid — NOT m_centre,
            // which is the LINE arm's projection onto the clicked line and is
            // meaningless here), dropped onto the plane.
            float c[3];
            const float d = m_hover.d;
            Mad3( m_mid, n, d - Dot3( n, m_mid ), c );

            const float arm = m_radius;        // KIWI-UX (CLEANUP, A-39)
            Copy3( c, m_p[0] );
            Mad3( c, e1, arm, m_p[1] );
            Mad3( c, e2, arm, m_p[2] );

            Copy3( n,  m_planeN );
            m_planeD = d;
            Copy3( e1, m_dir );
            Copy3( e2, m_away );
            Copy3( c,  m_centre );
            // The two "line" ends are meaningless for a face plane and DrawWorld
            // skips them; parked on the plane so nothing downstream reads garbage.
            Copy3( c, m_lineA );
            Copy3( c, m_lineB );
            return true;
        }

        // n = normalise( cross( lineDir, away ) ), away = the camera's forward
        // projected perpendicular to the line.  kiwi_split.h derives it in full.
        //
        // ROUND L: CALLED FROM EXACTLY ONE PLACE — Click(), at the instant the line
        // is picked.  Everything it writes (m_away, m_planeN, m_planeD, m_p) is the
        // gesture's locked plane from then on.  Nothing on a per-frame path may
        // call it; that was the bug.
        bool AimFromCamera()
        {
            CamWnd_BuildMatrix();
            const camera_s *cam = Ed_Camera();

            float away[3];
            Copy3( cam->vpn, away );
            const float along = Dot3( away, m_dir );
            Mad3( away, m_dir, -along, away );      // strip the component along the line
            if ( !Norm3( away ) )
                return false;                       // the line points at the camera

            float n[3];
            Cross3( m_dir, away, n );
            if ( !Norm3( n ) )
                return false;

            Copy3( away, m_away );
            Copy3( n, m_planeN );
            m_planeD = Dot3( n, m_lineA );

            // The three planepts the splitter is handed: two on the line (so the
            // cut passes exactly through it, which is the whole promise of the
            // feature) and one swept away from the camera.  Well spread by
            // construction — the sweep offset is a bounds-scaled distance, never
            // a hair.
            Copy3( m_lineA, m_p[0] );
            Copy3( m_lineB, m_p[1] );
            const float lift = m_radius;       // KIWI-UX (CLEANUP, A-39)
            Mad3( m_lineA, m_away, lift, m_p[2] );
            return true;
        }

        // The union bounds of the affected brushes: the preview's size and the
        // third planept's lever arm both come from it.
        // ═══════════════════════════════════════════════════════════════════
        //  KIWI-UX (ROUND AF, ITEM 1) — CUT WITH A CHAIN OR A LOOP
        // ═══════════════════════════════════════════════════════════════════
        // USER DIRECTIVE, verbatim: "When cutting, a lot of times I want to cut with
        // an entire circle or half-circle.  Allow shift-clicking of lines during cut
        // operation setup to enable this."
        //
        // ── WHAT A FENCE IS ────────────────────────────────────────────────
        // ONE construction object contributes ALL of its segments, not the one under
        // the cursor.  A circle is one object with N segments, so a plain click on a
        // circle already IS "the whole loop" — which is the directive's own example
        // and costs nothing to honour, because KiwiCon_SegmentCount / SegmentWorld
        // have always been able to enumerate it.  SHIFT+click ADDS another object's
        // segments; shift-clicking one that is already in takes it back out, the same
        // additive grammar the boolean's tool set uses.
        //
        // Each segment is then swept along ONE shared direction into a plane, and the
        // target brushes are split by every plane IN SEQUENCE, KEEPING BOTH HALVES
        // each time.  That is the only structural difference from the Q boolean's
        // cascade, which this borrows its shape from: the boolean discards the
        // intersection, and a cut discards nothing at all.
        //
        // ── THE SWEEP DIRECTION ─────────────────────────────────────────────
        // For a single segment it is unchanged, and deliberately so: AimFromCamera,
        // the round-L derivation, which strips the along-the-line component out of
        // the view direction so "the plane goes back into the screen" is what the
        // user sees.  That derivation is about ONE line and does not generalise — ask
        // it about a circle and it answers differently for every segment, producing a
        // fence of planes that fan instead of forming a tube.
        //
        // So a MULTI-SEGMENT fence uses the objects' own PLANE NORMAL
        // (KiwiCon_ObjectPlane).  A construction loop is planar by construction — it
        // is what the region layer accepts — and extruding it along its normal is
        // precisely the cookie cutter: a circle on the floor becomes a vertical tube,
        // and the brush inside the tube separates from the brush outside it.  If no
        // member carries a valid plane (two lines that determine none between them),
        // the camera derivation from the FIRST segment is the fallback and the console
        // says which was used.
        //
        // ── CONVEXITY, STATED PLAINLY ───────────────────────────────────────
        // Every piece a cut produces is CONVEX BY CONSTRUCTION — each split is a
        // single plane through a convex solid, and both halves of that are convex.
        // What a concave fence produces is not a concave piece, it is MANY pieces: an
        // L-shaped fence cutting a slab gives four solids, not two, because the two
        // planes of the L each cut the whole slab.  That is correct and it is what a
        // plane-defined editor can do; it is stated here, in the HUD and in
        // RADIANT_KNOWN_ISSUES so it is never mistaken for a bug.  The piece count is
        // bounded by 2^planes in the worst case, which is why KCUT_MAX_FENCE exists.
        //
        // ── MATERIALS AND UNDO ──────────────────────────────────────────────
        // Neither changes.  Every plane goes through KiwiSplit_DefByPlane, which
        // seeds its template face from kiwi_material.h R2/R3 against the def BEING
        // SPLIT — so a piece cut off a piece inherits from its own parent, which is
        // the rule stated positively.  ONE KiwiCmd_UndoBegin covers the whole
        // cascade, and the targets are on `selected_brushes`, so the bracket head
        // cloned all of them and nothing needs a hand cover.
        bool FenceActive() const { return (int)( m_fence.size() / 6 ) > 1; }
        int  FenceCount()  const { return (int)( m_fence.size() / 6 ); }

        bool FenceHasObject( int obj ) const
        {
            for ( size_t i = 0; i < m_fenceObjs.size(); ++i )
                if ( m_fenceObjs[i] == obj )
                    return true;
            return false;
        }

        // Rebuild m_fence (world segments) and m_fenceN (the sweep direction) from
        // m_fenceObjs.  Called after every change to the object set, so the two can
        // never disagree.  False = the set produces nothing usable.
        bool FenceRebuild()
        {
            m_fence.clear();
            m_fenceFromPlane = false;

            bool haveN = false;
            for ( size_t k = 0; k < m_fenceObjs.size(); ++k )
            {
                const kconObject_t *o = KiwiCon_At( m_fenceObjs[k] );
                if ( !o || o->hidden )
                    continue;

                // The sweep direction comes from the FIRST member that has a plane.
                // First rather than "best": the order is the pick order, which is the
                // one the mapper can predict, and the members are coplanar in every
                // case this is meant for.
                if ( !haveN )
                {
                    kconPlane_t pl;
                    if ( KiwiCon_ObjectPlane( *o, &pl ) )
                    {
                        Copy3( pl.normal, m_fenceN );
                        haveN = Norm3( m_fenceN );
                        m_fenceFromPlane = haveN;
                    }
                }

                const int segs = KiwiCon_SegmentCount( *o );
                for ( int s = 0; s < segs; ++s )
                {
                    if ( FenceCount() >= KCUT_MAX_FENCE )
                    {
                        Sys_Printf( "Cut: that is more than %i segments — the fence is "
                                    "capped there (a cut by N planes can make 2^N "
                                    "pieces).  Use a coarser circle, or cut twice.\n",
                                    KCUT_MAX_FENCE );
                        return FenceCount() > 0;
                    }
                    float wa[3], wb[3];
                    if ( !KiwiCon_SegmentWorld( *o, s, wa, wb ) )
                        break;
                    float e[3];
                    Sub3( wb, wa, e );
                    if ( Len3( e ) < 1.0e-3f )
                        continue;               // a zero segment names no plane
                    for ( int c = 0; c < 3; ++c ) m_fence.push_back( wa[c] );
                    for ( int c = 0; c < 3; ++c ) m_fence.push_back( wb[c] );
                }
            }

            if ( m_fence.empty() )
                return false;

            if ( !haveN )
            {
                // No member carries a plane.  Fall back to the single-line camera
                // derivation over the FIRST segment — which is exactly what the
                // one-segment cut would have done, so the degenerate case degrades
                // into the behaviour that has always been there.
                Copy3( &m_fence[0], m_lineA );
                Copy3( &m_fence[3], m_lineB );
                Sub3( m_lineB, m_lineA, m_dir );
                if ( !Norm3( m_dir ) || !AimFromCamera() )
                    return false;
                Copy3( m_away, m_fenceN );
            }
            return true;
        }

        // The three planepts of fence plane `i`: the segment's two ends plus one end
        // swept along the fence direction.  Spread by the targets' own radius for the
        // reason MeasureBounds states — a cross product of nearly-coincident
        // differences is precision the .map round-trip cannot afford.
        bool FencePlanePts( int i, float out[3][3] ) const
        {
            if ( i < 0 || i >= FenceCount() )
                return false;
            const float *a = &m_fence[(size_t)i * 6];
            const float *b = &m_fence[(size_t)i * 6 + 3];
            float e[3];
            Sub3( b, a, e );
            if ( !Norm3( e ) )
                return false;
            // A segment PARALLEL to the sweep names no plane — its extrusion is a
            // line.  Refused per-plane rather than for the whole fence, because one
            // bad segment in a fifty-segment ring is not a reason to refuse the ring.
            float n[3];
            Cross3( e, m_fenceN, n );
            if ( !Norm3( n ) )
                return false;

            const float lift = m_radius;       // KIWI-UX (CLEANUP, A-39)
            Copy3( a, out[0] );
            Copy3( b, out[1] );
            Mad3( a, m_fenceN, lift, out[2] );
            return true;
        }

        // THE CASCADE.  One target at a time, all-or-nothing per target, nothing
        // landed until every plane has been applied to it.
        //
        // OWNERSHIP is the boolean's, verbatim, and the invariant is the same:
        // `!owned  <=>  cur == { the map's own def }`, which is borrowed and must
        // never be freed.  The transition is one-way and happens the first time a
        // plane actually divides something, at which point `cur` held exactly the one
        // borrowed def and it was replaced wholesale.
        //
        // Returns the number of TARGETS that were cut; `outPieces` is how many solids
        // they became.
        int CascadeCut( const std::vector<selbrush_t *> &targets, int *outPieces )
        {
            struct work_t
            {
                selbrush_t            *node;
                std::vector<brush_t *> pieces;
            };
            std::vector<work_t> work;
            int refused = 0;

            for ( size_t t = 0; t < targets.size(); ++t )
            {
                selbrush_t *node = targets[t];
                if ( !Sel_BrushLive( node ) || !node->def )
                    continue;

                std::vector<brush_t *> cur;
                bool owned = false;
                cur.push_back( node->def );

                bool bad = false;
                for ( int p = 0; p < FenceCount() && !bad; ++p )
                {
                    float pts[3][3];
                    if ( !FencePlanePts( p, pts ) )
                        continue;               // degenerate plane: skip, not fatal

                    std::vector<brush_t *> next;
                    bool changed = false;

                    for ( size_t c = 0; c < cur.size(); ++c )
                    {
                        brush_t    *front = 0, *back = 0;
                        const char *why   = "unknown";
                        if ( !KiwiSplit_DefByPlane( cur[c], pts[0], pts[1], pts[2],
                                                    &front, &back, &why ) )
                        {
                            // Nothing was allocated by that call.  Ours is `next`
                            // plus the part of `cur` this pass has not disposed of
                            // yet — which starts at c, NOT at 0: everything earlier
                            // was either freed or handed to `next`.  All of it guarded
                            // on owning the set at all.
                            if ( owned )
                            {
                                for ( size_t q = c; q < cur.size(); ++q )
                                    KiwiSplit_FreeUnlandedDef( cur[q] );
                                for ( size_t q = 0; q < next.size(); ++q )
                                    KiwiSplit_FreeUnlandedDef( next[q] );
                            }
                            Sys_Printf( "Cut: one brush left untouched — %s.\n",
                                        why ? why : "invalid geometry" );
                            ++refused;
                            bad = true;
                            break;
                        }

                        if ( front && back )
                        {
                            // THE SPLIT.  Both halves survive — this is the whole
                            // difference from the boolean, which drops the `back`.
                            next.push_back( front );
                            next.push_back( back );
                            if ( owned )
                                KiwiSplit_FreeUnlandedDef( cur[c] );
                            changed = true;
                        }
                        else
                        {
                            // The plane missed this piece.  Whichever half came back
                            // is a re-clone of the same volume; drop it and carry the
                            // piece we already have.
                            if ( front ) KiwiSplit_FreeUnlandedDef( front );
                            if ( back  ) KiwiSplit_FreeUnlandedDef( back );
                            next.push_back( cur[c] );
                        }
                    }

                    if ( bad )
                        break;
                    cur.swap( next );
                    if ( changed )
                        owned = true;
                }

                if ( bad )
                    continue;
                if ( !owned )
                    continue;                   // every plane missed: leave it alone

                work.push_back( work_t() );
                work.back().node = node;
                work.back().pieces.swap( cur );
            }

            int landed = 0;
            for ( size_t w = 0; w < work.size(); ++w )
            {
                // ORDER (kiwi_split.h UNDO): land every piece FIRST, then free the
                // source, so the owner entity never transiently drops to zero brushes.
                for ( size_t p = 0; p < work[w].pieces.size(); ++p )
                {
                    selbrush_t *inst = Brush_AddToList( work[w].pieces[p],
                                                        work[w].node->owner );
                    if ( inst->next || inst->prev )
                        Com_Error( ERR_FATAL, "Brush_AddToList: already linked" );
                    Brush_AddToList2( inst );
                    ++landed;
                }
                Brush_Free( work[w].node );
            }

            if ( outPieces )
                *outPieces = landed;
            return (int)work.size();
        }

        void MeasureBounds()
        {
            float lo[3] = {  1e30f,  1e30f,  1e30f };
            float hi[3] = { -1e30f, -1e30f, -1e30f };
            for ( size_t i = 0; i < m_targets.size(); ++i )
            {
                // KIWI-UX (CLEANUP, A-24): m_targets is latched in Begin() and this
                // runs from Click(), so a brush can have been freed under a paused
                // gesture — the same reason Commit/CountCrossings/DrawWorld re-test.
                if ( !Sel_BrushLive( m_targets[i] ) || !m_targets[i]->def )
                    continue;
                const brush_t *def = m_targets[i]->def;
                for ( int k = 0; k < 3; ++k )
                {
                    if ( def->mins[k] < lo[k] ) lo[k] = def->mins[k];
                    if ( def->maxs[k] > hi[k] ) hi[k] = def->maxs[k];
                }
            }
            float diag[3];
            Sub3( hi, lo, diag );
            m_radius = Len3( diag ) * 0.5f;
            if ( !( m_radius > 1.0f ) )
                m_radius = 64.0f;

            // The bounds centre, kept as its own member: the LINE arm projects it
            // onto the clicked line (below) and ROUND T's FACE arm drops it onto the
            // clicked plane, so both need the unprojected point.
            for ( int k = 0; k < 3; ++k )
                m_mid[k] = ( lo[k] + hi[k] ) * 0.5f;

            // The quad is centred on the LINE, lifted onto the span the brushes
            // occupy along it, so a short line still previews a full-width cut.
            float rel[3];
            Sub3( m_mid, m_lineA, rel );
            Mad3( m_lineA, m_dir, Dot3( rel, m_dir ), m_centre );
            // (ROUND L: this used to end with a second AimFromCamera() to re-derive
            //  the third planept now that the lever arm was known.  The caller runs
            //  MeasureBounds THEN AimFromCamera, in that order, exactly once, so the
            //  re-derivation is the caller's next line instead of a hidden one here.)
        }

        // How many of the targets the LOCKED plane crosses.  Counted once, when the
        // plane is locked — it cannot change without the brushes changing, and the
        // commit re-tests anyway.
        void CountCrossings()
        {
            m_crossing = 0;
            for ( size_t i = 0; i < m_targets.size(); ++i )
                if ( Sel_BrushLive( m_targets[i] )
                  && KiwiSplit_PlaneCrossesBrush( m_targets[i]->def, m_p[0], m_p[1], m_p[2] ) )
                    ++m_crossing;
        }

        void UpdateHud()
        {
            if ( m_stage == KCUT_PICK_LINE )
            {
                // ROUND T: NAME the thing the click would take, so "why did it use
                // that plane" is answered before the click rather than after it.
                const char *what = "click a line, edge or face";
                if ( m_haveHover )
                    what = ( m_hover.kind == KCUT_SRC_FACE ) ? "click this FACE — its plane cuts"
                         : ( m_hover.kind == KCUT_SRC_EDGE ) ? "click this EDGE — sweeps from the view"
                    // ROUND AF, ITEM 1: a construction click takes the WHOLE object,
                    // and the chip has to say so — "click this LINE" is a promise the
                    // tool no longer keeps for a circle.
                                                             : "click this SHAPE — all of it cuts";
                _snprintf( m_hud, sizeof( m_hud ),
                           "cut  %i brush(es)  %s", (int)m_targets.size(), what );
            }
            else
            {
                if ( FenceActive() )
                    // ROUND AF, ITEM 1.  The piece count is what a mapper needs to
                    // brace for: a concave fence makes MANY solids and that is
                    // correct, so the number is advertised rather than discovered.
                    _snprintf( m_hud, sizeof( m_hud ),
                               "cut  FENCE %i planes  %i of %i brush(es) crossed  "
                               "(nothing is discarded)",
                               FenceCount(), m_crossing, (int)m_targets.size() );
                else
                _snprintf( m_hud, sizeof( m_hud ),
                           "cut  %i of %i brush(es) crossed  (%s plane locked)",
                           m_crossing, (int)m_targets.size(),
                           ( m_srcKind == KCUT_SRC_FACE ) ? "face"
                         : ( m_srcKind == KCUT_SRC_EDGE ) ? "edge" : "line" );
            }
            m_hud[sizeof( m_hud ) - 1] = '\0';
        }

        // ROUND L: which half of the flow is running (kiwi_split.h THE TWO-STAGE CUT).
        enum stage_t { KCUT_PICK_LINE = 0, KCUT_PREVIEW };
        stage_t   m_stage     = KCUT_PICK_LINE;
        bool      m_haveHover = false;
        cutPick_t m_hover;
        // ROUND X, ITEM 7: the disc's centre — the live snap point, latched in
        // MouseMove.  Separate from m_hover because a snap can be valid over empty
        // space where no cut source is hovered, and the disc still says where the
        // cursor is.
        bool      m_haveDisc = false;
        float     m_discPt[3] = { 0.0f, 0.0f, 0.0f };
        cutSrc_t  m_srcKind   = KCUT_SRC_LINE;   // ROUND T: what the LOCKED plane came from

        std::vector<selbrush_t *> m_targets;
        float m_lineA[3]  = { 0.0f, 0.0f, 0.0f };
        float m_lineB[3]  = { 0.0f, 0.0f, 0.0f };
        float m_dir[3]    = { 1.0f, 0.0f, 0.0f };
        float m_away[3]   = { 0.0f, 1.0f, 0.0f };
        float m_centre[3] = { 0.0f, 0.0f, 0.0f };
        float m_mid[3]    = { 0.0f, 0.0f, 0.0f };   // ROUND T: the targets' bounds centre
        float m_planeN[3] = { 0.0f, 0.0f, 1.0f };
        float m_planeD    = 0.0f;
        float m_p[3][3]   = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } };
        // KIWI-UX (CLEANUP, A-39): INVARIANT — m_radius is always > 1.
        // It starts at 64 and MeasureBounds is its only writer, which ends with
        // `if ( !( m_radius > 1.0f ) ) m_radius = 64.0f;`.  Four downstream
        // lever-arm sites used to re-spell that clamp as
        // `( m_radius > 1.0f ) ? m_radius : 64.0f`, which could never take its
        // second arm; they read m_radius directly now.  Anything that becomes a
        // second writer must keep this invariant or restore those guards.
        float m_radius    = 64.0f;
        // ── ROUND AF, ITEM 1: the FENCE (see the block above MeasureBounds) ──
        // The construction objects the cut is made of, in PICK ORDER (Shift adds,
        // a shift-click on one already in takes it out), and the world segments
        // they flatten to — 6 floats per segment, rebuilt whole by FenceRebuild so
        // the two can never drift.  `m_fenceN` is the shared sweep direction and
        // `m_fenceFromPlane` says whether it came from the objects own plane or
        // from the camera fallback, which is the one thing the console has to say.
        std::vector<int>   m_fenceObjs;
        std::vector<float> m_fence;
        float m_fenceN[3] = { 0.0f, 0.0f, 1.0f };
        bool  m_fenceFromPlane = false;
        int   m_crossing  = 0;
        char  m_hud[128]  = { 0 };
    };

    // ROUND S: the one numeric field the live split gets.  STATIC storage — the
    // numeric layer copies the struct but never the label (kiwi_numeric.h).  It
    // must stay a table of ONE: two fields would take Tab away from the U/V flip
    // (kiwi_split.h TAB ROUTING).
    const kiwiNumField_t KSPLITFACE_FIELDS[1] = { { "offset", KNUM_LENGTH, false } };

    // ═════════════════════════════════════════════════════════════════════════
    //  Ctrl+R — FACE SPLIT.  One selected face, a LIVE line across it that
    //  follows the cursor, TAB flips U/V.  See kiwi_split.h ROUND S.
    // ═════════════════════════════════════════════════════════════════════════
    class KiwiSplitFaceCommand : public KiwiEditorCommand
    {
    public:
        const char *Name() const override { return "Split Face"; }
        bool CanExecute() override { return KiwiSplit_CanSplitFace(); }

        // ROUND S: ONE field — the offset of the cut from the face edge the slide
        // axis points AWAY from.  Exactly one, because the Tab rung is
        // `KiwiNum_FieldCount() <= 1` (kiwi_split.h TAB ROUTING).
        int NumericFields( const kiwiNumField_t **out ) const override
        { *out = KSPLITFACE_FIELDS; return 1; }

        // The LIVE offset, so the §13b value bubble shows where the cut is even
        // when nothing has been typed.
        bool NumericFieldValue( int field, float *out ) const override
        {
            if ( field != 0 || !out || !m_valid )
                return false;
            *out = m_t - m_lo;
            return true;
        }

        // Pin the bubble to the cut LINE's midpoint — the thing the number is about.
        bool BubbleAnchor( float *out3 ) const override
        {
            if ( !out3 || !m_valid )
                return false;
            for ( int k = 0; k < 3; ++k )
                out3[k] = ( m_a[k] + m_b[k] ) * 0.5f;
            return true;
        }

        // Typing takes the line off the cursor until the field is cleared.
        void NumericChanged( bool has, float world ) override
        {
            m_hasNum   = has && KiwiNum_HasValue();
            m_numWorld = world;
            Derive();
            UpdateHud();
            g_nUpdateBits |= 1;
        }

        const char *HudStatus() const override { return m_hud[0] ? m_hud : 0; }
        bool        HudInvalid() const override { return !m_valid; }

        // ROUND S: the tech limitation, on the prompt strip where it is read
        // BEFORE the commit rather than in a console line after it.
        int HudPrompts( const kiwiPrompt_t **out ) const override
        {
            static const kiwiPrompt_t s_prompts[] = {
                { "Tab",  "Flip U / V" },
                { "Move", "Slide the cut" },
                // ROUND Y, ITEM 3: say the centre spot exists.  It is drawn and it
                // latches, and a snap nobody knows about is a snap nobody uses.
                { "Dot",  "Face centre - snaps for an exact half" },
                { "0-9",  "Exact offset" },
                { "!",    "Splits the brush - brush faces cannot split alone" },
            };
            *out = s_prompts;
            return (int)( sizeof( s_prompts ) / sizeof( s_prompts[0] ) );
        }

        bool Begin() override
        {
            m_valid = false;
            m_axis  = 0;                    // U
            m_hud[0] = '\0';
            m_hasNum   = false;
            m_numWorld = 0.0f;
            m_haveT    = false;             // seed at the centroid until the mouse moves

            const selection_t &sel = KiwiSel();
            const sel_item_t  *face = 0;
            int                nFaces = 0;
            for ( size_t i = 0; i < sel.items.size(); ++i )
            {
                if ( sel.items[i].kind != SEL_FACE || !Sel_BrushLive( sel.items[i].brush ) )
                    continue;
                face = &sel.items[i];
                ++nFaces;
            }
            if ( nFaces != 1 || !face )
            {
                Sys_Printf( "Split Face: select exactly ONE face.\n" );
                return false;
            }
            if ( !Splittable( face->brush ) )
            {
                Sys_Printf( "Split Face: that brush cannot be split.\n" );
                return false;
            }
            m_node = face->brush;
            m_face = face->faceIndex;
            if ( !Derive() )
            {
                Sys_Printf( "Split Face: that face is degenerate.\n" );
                return false;
            }
            UpdateHud();
            // ROUND S: say the tech limitation ONCE, in the words the directive
            // asked about, so "it only splits the face" is answered before the
            // first commit rather than discovered after it.
            Sys_Printf( "Split Face: move the mouse to slide the cut, Tab flips U/V, "
                        "type an exact offset, RMB / Enter splits.\n"
                        "Split Face: this splits the BRUSH along that line - a brush "
                        "face cannot be split on its own (a convex plane-brush has no "
                        "way to carry a divided face).\n" );
            return true;
        }

        // TAB flips U/V.  Reached because kiwi_command.cpp offers Tab to the active
        // command BEFORE the numeric layer whenever the numeric layer has at most
        // one field to cycle (kiwi_split.h TAB ROUTING).
        bool KeyDown( int vk, unsigned mods ) override
        {
            (void)mods;
            if ( vk != 0x09 )               // VK_TAB
                return false;
            m_axis = ( m_axis + 1 ) & 1;
            // The offset's REFERENCE EDGE changes with the axis, so a carried-over
            // position (typed or hovered) would silently mean something else.  Drop
            // both and re-seat at the centroid; the next move picks it straight up.
            m_haveT = false;
            Derive();
            UpdateHud();
            g_nUpdateBits |= 1;
            return true;
        }

        // ── ROUND S: THE LIVE CURSOR MAPPING ─────────────────────────────────
        // The whole of "let me customize it".  See kiwi_split.h for the rule; the
        // three sources in priority order are: a TYPED offset (handled in Derive),
        // a GEOMETRY snap that lies on the face plane, and the cursor ray's own
        // intersection with the face plane, grid-snapped.
        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick;
            // The basis FIRST: OffDir() below is the slide axis, and it has to be
            // this frame's (Tab, or a brush edited under a paused gesture, moves it).
            float n[3], d;
            if ( DeriveBasis() && FacePlane( n, &d ) )
            {
                float world[3];
                bool  have = false;

                // (1) a geometry snap ON THIS FACE'S PLANE.  The plane test is what
                //     keeps a vertex on some other brush from yanking the cut: the
                //     snap layer ranks globally, this command is about one face.
                //
                //     SNAP_FACE IS DELIBERATELY EXCLUDED.  It is the one "geometry"
                //     type that is not a target the user aimed at — arm 6 is the
                //     ported Test_Ray surface hit, UNSNAPPED (kiwi_snap.h says so in
                //     those words), and it fires on the very face being split.
                //     Taking it would mean the cut NEVER grid-snapped, because the
                //     grid arm below would never be reached.  Every other geometry
                //     type is a point or an edge someone placed or built.
                if ( snap.valid && KiwiSnap_IsGeometry( snap.type )
                  && snap.type != SNAP_FACE
                  && fabsf( Dot3( n, snap.position ) - d ) <= KSPLIT_FACE_EPS )
                {
                    Copy3( snap.position, world );
                    have = true;
                }
                // (2) the cursor ray ∩ the face plane, then the §17 lattice.
                //     CTRL SUPPRESSES SNAPPING (§6, kiwi_snap.h arm 0): the layer
                //     answers SNAP_NONE, and SNAP_NONE is produced by nothing else
                //     on a valid query (arm 9 always answers SNAP_GRID), so it is
                //     the signal to leave the plane hit exactly where it is.
                if ( !have )
                {
                    ray_t ray;
                    float hit[3];
                    if ( Pick_RayFromCursor( &ray ) && RayPlane( ray, n, d, hit ) )
                    {
                        const bool suppressed = snap.valid && snap.type == SNAP_NONE;
                        float snapped[3];
                        if ( !suppressed && KiwiGrid_Snap( hit, snapped ) )
                            Copy3( snapped, world );
                        else
                            Copy3( hit, world );
                        have = true;
                    }
                }
                if ( have )
                {
                    m_cursorT = Dot3( world, OffDir() );
                    m_haveT   = true;

                    // ── ROUND Y, ITEM 3: THE CENTRE LATCH ────────────────────
                    // The face centroid is the "split it exactly in half" answer
                    // and it is now drawn (DrawWorld below), so it has to be
                    // reachable too — a marked target the cursor slides straight
                    // past is worse than no mark.  Measured in SCREEN PIXELS at
                    // the centroid's own depth, so the catch is the same size at
                    // every zoom, and applied LAST so a real geometry snap
                    // (arm 1) still wins: that is a target the user aimed at.
                    //
                    // CTRL SUPPRESSES IT, on the same signal arm 2 uses: the snap
                    // layer answers SNAP_NONE only when §6's suppression is on.
                    const bool suppressed = snap.valid && snap.type == SNAP_NONE;
                    const bool tookGeometry = snap.valid && KiwiSnap_IsGeometry( snap.type )
                                           && snap.type != SNAP_FACE
                                           && fabsf( Dot3( n, snap.position ) - d ) <= KSPLIT_FACE_EPS;
                    if ( !suppressed && !tookGeometry && !m_hasNum )
                    {
                        const float wpp  = KiwiCam_WorldPerPixel( m_centre );
                        const float band = KSPLIT_CENTRE_SNAP_PIX * wpp;
                        if ( band > 0.0f && fabsf( m_cursorT - m_centreT ) <= band )
                            m_cursorT = m_centreT;
                    }
                }
            }
            Derive();
            UpdateHud();
        }

        void DrawWorld() override
        {
            if ( !m_valid )
                return;
            KiwiLines_Color( KSPLIT_EDGE_COL[0], KSPLIT_EDGE_COL[1], KSPLIT_EDGE_COL[2] );
            KiwiLines_Add( m_a, m_b );
            KiwiLines_Color( KSPLIT_LINE_COL[0], KSPLIT_LINE_COL[1], KSPLIT_LINE_COL[2] );
            if ( Sel_BrushLive( m_node ) )
                DrawIntersection( m_node->def, m_planeN, m_planeD );

            // ── ROUND Y, ITEM 3: THE CENTRE SPOT, PERSISTENTLY ──────────────
            // Drawn LAST so it sits over the cut line rather than under it, and
            // in its own colour run (kiwi_lines.h — a colour change opens a new
            // run, so the two glyphs are emitted together after every line).
            // Dot + separated ring is kiwi_snap.cpp's POINT glyph, which is what
            // this is: a place the cut can land on.  Nine + twelve segments, well
            // inside KiwiCmd_DrawWorld's budget.
            KiwiLines_Color( KSPLIT_CENTRE_COL[0], KSPLIT_CENTRE_COL[1], KSPLIT_CENTRE_COL[2] );
            KiwiSnap_EmitSpot( m_centre, KiwiSnap_AccentPixels() * 2.0f, true  );
            KiwiSnap_EmitSpot( m_centre, KiwiSnap_RingPixels(),          false );
        }

        void Commit() override
        {
            if ( !m_valid || !Sel_BrushLive( m_node ) )
            {
                Sys_Printf( "Split Face: nothing to split.\n" );
                return;
            }

            // The face selection is NOT on selected_brushes (kiwi_selection.h
            // DESIGN NOTE 2), so the bracket head's Undo_AddBrushList would clone
            // NOTHING and undo could not bring the source brush back.  Put the
            // brush on the legacy selection FIRST, exactly the way the ported
            // Select_Brush does it, and the whole CSG_MakeHollow bracket shape then
            // applies unchanged: the head clones it, the tail stamps the halves.
            Select_Deselect( 1 );
            Select_Brush( m_node, 0, 0, 0 );
            Sel_Clear( KiwiSel() );

            KiwiCmd_UndoBegin( "split face" );
            std::vector<selbrush_t *> one;
            one.push_back( m_node );
            const int n = SplitTargets( one, m_p[0], m_p[1], m_p[2], "Split Face" );
            if ( n )
                Sys_Printf( "Split Face: 1 brush -> 2.\n" );

            m_valid = false;
            g_nUpdateBits = -1;
        }

        void Cancel() override
        {
            m_valid = false;
            g_nUpdateBits |= 1;
        }

    private:
        // ── the FACE's own plane, as (n, d) ─────────────────────────────────
        // Read from face_t::plane, normalised defensively (a face whose winding
        // survives Brush_BuildWindings always has a unit normal, but this runs on
        // every mouse move and a NaN here would poison the whole gesture).
        bool FacePlane( float n[3], float *d ) const
        {
            if ( !Sel_BrushLive( m_node ) || !m_node->def || !m_node->def->faces )
                return false;
            if ( m_face < 0 || m_face >= m_node->def->faceCount )
                return false;
            const face_t *f = &m_node->def->faces[m_face];
            Copy3( f->plane.normal, n );
            if ( !Norm3( n ) )
                return false;
            const winding_t *w = f->w;
            if ( !w || w->numpoints < 3 )
                return false;
            *d = Dot3( n, w->p[0] );        // the winding IS on the plane
            return true;
        }

        // Where the cursor ray meets that plane.  False when the ray is parallel
        // to it (an edge-on face — a transient the user orbits out of).
        static bool RayPlane( const ray_t &ray, const float n[3], float d, float out[3] )
        {
            const float denom = Dot3( n, ray.dir );
            if ( fabsf( denom ) < 1.0e-5f )
                return false;
            const float t = ( d - Dot3( n, ray.origin ) ) / denom;
            Mad3( ray.origin, ray.dir, t, out );
            return true;
        }

        // The SLIDE AXIS: in the face plane, perpendicular to the cut line.  The
        // basis is re-derived every frame (the brush can change under a paused
        // gesture), so this reads the cached copy DeriveBasis wrote.
        const float *OffDir() const { return m_off; }

        // Basis + span, with no reference to where the cut currently is.  Fills
        // m_lineDir / m_off / m_lo / m_hi / m_half / m_nrm.
        bool DeriveBasis()
        {
            if ( !Sel_BrushLive( m_node ) || !m_node->def || !m_node->def->faces )
                return false;
            if ( m_face < 0 || m_face >= m_node->def->faceCount )
                return false;
            const face_t    *f = &m_node->def->faces[m_face];
            const winding_t *w = f->w;
            if ( !w || w->numpoints < 3 || w->numpoints > MAX_POINTS_ON_WINDING )
                return false;

            float c[3] = { 0.0f, 0.0f, 0.0f };
            for ( int i = 0; i < w->numpoints; ++i )
                for ( int k = 0; k < 3; ++k )
                    c[k] += w->p[i][k];
            const float inv = 1.0f / (float)w->numpoints;
            for ( int k = 0; k < 3; ++k )
                c[k] *= inv;

            // The longest winding edge gives U's reference direction, and also the
            // half-length the preview line is drawn at.
            float longest[3] = { 0.0f, 0.0f, 0.0f };
            float bestLen    = 0.0f;
            float radius     = 0.0f;
            for ( int i = 0; i < w->numpoints; ++i )
            {
                const int j = ( i + 1 ) % w->numpoints;
                float e[3];
                Sub3( w->p[j], w->p[i], e );
                const float l = Len3( e );
                if ( l > bestLen ) { bestLen = l; Copy3( e, longest ); }
                float rel[3];
                Sub3( w->p[i], c, rel );
                const float r = Len3( rel );
                if ( r > radius ) radius = r;
            }
            if ( !Norm3( longest ) || !( radius > KSPLIT_EPS ) )
                return false;

            Copy3( f->plane.normal, m_nrm );
            if ( !Norm3( m_nrm ) )
                return false;

            // U = perpendicular to the longest edge, in the face plane; V = along
            // it.  A cut ACROSS the long axis is what "split it in 2" means for a
            // long face, which is why U is the default.
            float u[3];
            Cross3( m_nrm, longest, u );
            if ( !Norm3( u ) )
                return false;
            Copy3( ( m_axis == 0 ) ? u : longest, m_lineDir );

            // The slide axis, and the face's extent along it.
            Cross3( m_nrm, m_lineDir, m_off );
            if ( !Norm3( m_off ) )
                return false;
            m_lo =  1e30f;
            m_hi = -1e30f;
            for ( int i = 0; i < w->numpoints; ++i )
            {
                const float s = Dot3( m_off, w->p[i] );
                if ( s < m_lo ) m_lo = s;
                if ( s > m_hi ) m_hi = s;
            }
            if ( !( m_hi - m_lo > KSPLIT_MIN_SPAN ) )
                return false;               // a sliver: no room for two halves

            m_half     = radius * 1.05f;
            m_centreT  = Dot3( m_off, c );
            Copy3( c, m_centre );
            return true;
        }

        // Basis + the CURRENT position -> the preview line and the cut plane.
        // Position priority (kiwi_split.h): a TYPED offset, else the last cursor
        // projection, else the centroid (the pre-round-S 50/50).
        bool Derive()
        {
            m_valid = false;
            if ( !DeriveBasis() )
                return false;

            float t;
            if ( m_hasNum )
                t = m_lo + m_numWorld;      // offset FROM the low edge (see the header)
            else if ( m_haveT )
                t = m_cursorT;
            else
                t = m_centreT;

            // Both halves must survive: keep the cut strictly inside the span.
            const float guard = KSPLIT_MIN_SPAN * 0.5f;
            if ( t < m_lo + guard ) t = m_lo + guard;
            if ( t > m_hi - guard ) t = m_hi - guard;
            m_t = t;

            // The line: through the centroid slid onto `t` along the slide axis.
            float pc[3];
            Mad3( m_centre, m_off, t - m_centreT, pc );
            Mad3( pc, m_lineDir, -m_half, m_a );
            Mad3( pc, m_lineDir,  m_half, m_b );

            // planepts: the two line ends plus one lifted along the face normal.
            Copy3( m_a, m_p[0] );
            Copy3( m_b, m_p[1] );
            Mad3( m_a, m_nrm, ( m_half > 1.0f ) ? m_half : 16.0f, m_p[2] );

            if ( !PlaneOf( m_p[0], m_p[1], m_p[2], m_planeN, &m_planeD ) )
                return false;
            m_valid = true;
            return true;
        }

        void UpdateHud()
        {
            if ( !m_valid )
            {
                _snprintf( m_hud, sizeof( m_hud ),
                           "split face  direction %s  (no room to split)",
                           ( m_axis == 0 ) ? "U" : "V" );
            }
            else
            {
                // The number and its SCALE together: "offset 32 / 128" says both
                // where the cut is and what it is measured against, which is what
                // makes the reference edge legible without a second line of prose.
                _snprintf( m_hud, sizeof( m_hud ),
                           "split brush  %s  offset %.6g / %.6g%s  (Tab flips)",
                           ( m_axis == 0 ) ? "U" : "V",
                           Units_ToDisplay( m_t - m_lo ),
                           Units_ToDisplay( m_hi - m_lo ),
                           m_hasNum ? "  [typed]" : "" );
            }
            m_hud[sizeof( m_hud ) - 1] = '\0';
        }

        selbrush_t *m_node = 0;
        int   m_face   = -1;
        int   m_axis   = 0;                 // 0 = U, 1 = V
        bool  m_valid  = false;
        // ROUND S: the live position.  m_cursorT/m_haveT are the cursor's own
        // projection onto the slide axis; m_hasNum/m_numWorld are the typed offset
        // (which outranks it); m_t is what Derive settled on.
        bool  m_haveT     = false;
        float m_cursorT   = 0.0f;
        bool  m_hasNum    = false;
        float m_numWorld  = 0.0f;
        float m_t         = 0.0f;
        // Basis + span, rebuilt by DeriveBasis every frame.
        float m_lineDir[3] = { 1.0f, 0.0f, 0.0f };
        float m_off[3]     = { 0.0f, 1.0f, 0.0f };
        float m_nrm[3]     = { 0.0f, 0.0f, 1.0f };
        float m_centre[3]  = { 0.0f, 0.0f, 0.0f };
        float m_centreT    = 0.0f;
        float m_lo         = 0.0f;
        float m_hi         = 0.0f;
        float m_half       = 16.0f;
        float m_a[3]      = { 0.0f, 0.0f, 0.0f };
        float m_b[3]      = { 0.0f, 0.0f, 0.0f };
        float m_p[3][3]   = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } };
        float m_planeN[3] = { 0.0f, 0.0f, 1.0f };
        float m_planeD    = 0.0f;
        char  m_hud[160]  = { 0 };
    };

    KiwiCutCommand       s_cut;
    KiwiSplitFaceCommand s_splitFace;
}

// ─── §3 canExecute ───────────────────────────────────────────────────────────
// ROUND L: ">= 1 selected brush", and nothing else.  USER DIRECTIVE: "It should
// be: Select a solid, press C, then the selection expects a line to be selected."
// The line is picked INSIDE the gesture now, so requiring one up front here would
// grey the palette row for the exact workflow the directive describes.
bool KiwiSplit_CanCut()
{
    for ( selbrush_t *n = selected_brushes.next; n != &selected_brushes; n = n->next )
        if ( Splittable( n ) )
            return true;
    return false;
}

bool KiwiSplit_CanSplitFace()
{
    const selection_t &sel = KiwiSel();
    int n = 0;
    // KIWI-UX (CLEANUP, A-25): the SAME test Begin() runs (:2256).  Without the
    // liveness gate the palette advertises Split Face for a face whose brush was
    // freed, and Begin then refuses with "select exactly ONE face".
    for ( size_t i = 0; i < sel.items.size(); ++i )
        if ( sel.items[i].kind == SEL_FACE && Sel_BrushLive( sel.items[i].brush ) )
            ++n;
    return n == 1;
}

// ─── registration + lookup ───────────────────────────────────────────────────
void KiwiSplit_RegisterCommands()
{
    extern bool Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId );
    Radiant_RegisterCommand( "KiwiCut",       0, 0, KIWI_CMD_CUT );
    Radiant_RegisterCommand( "KiwiSplitFace", 0, 0, KIWI_CMD_SPLIT_FACE );
}

KiwiEditorCommand *KiwiSplit_CommandForId( int commandId )
{
    if ( commandId == KIWI_CMD_CUT )
        return &s_cut;
    if ( commandId == KIWI_CMD_SPLIT_FACE )
        return &s_splitFace;
    return 0;
}

#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_matchface.cpp — SHAKEOUT G implementation.  See kiwi_matchface.h for the
// Plasticity finding (there is no such verb there), the plane-copy definition,
// the orientation rule and the TRIAL that keeps a rejection off the undo stack.
//
// NEW code over the ported cores: the plane math and the pick flow are new, the
// rebuild / gate / texture-lock calls are the ported ones §20 already uses.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"        // camera_s
#include "prefs.h"          // g_PrefsDlg (texture / lightmap lock)

#include "kiwi_matchface.h"
#include "kiwi_command.h"
#include "kiwi_lines.h"
#include "kiwi_pick.h"
#include "kiwi_selection.h"
#include "kiwi_validity.h"
#include "kiwi_vec.h"     // KIWI-UX (CLEANUP, A-15): the one spelling of Dot3/Sub3/...

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ── ported entry points (each verified against its definition) ──────────────
extern int   Sys_Printf( const char *fmt, ... );                       // win_qe3.cpp
extern int   g_nUpdateBits;                                            // 0x25D5A74 (mainfrm.cpp)
extern int   Face_MakePlane( face_t *face );                           // brush.cpp:4495 (0x470470)
// ROUND AA, ITEM 4 — the planarize-away path.  Declaration copied from
// kiwi_bevel.cpp's extern block, which is the other verb that drops a half-space.
extern unsigned int Brush_RemoveFace( brush_t *b, unsigned int faceIndex );   // brush.cpp:345  0x471640
// KIWI-UX (CLEANUP, B-28): FILE SCOPE, not block scope.  Round AI shipped a link
// error from a block-scope extern that MSVC mangled with its enclosing namespace;
// kiwi_uv.cpp carries the full account.  This is the declaration that used to sit
// inside KiwiMatch_RegisterCommands.
extern bool  Radiant_RegisterCommand( const char *name, byte vk, byte mods,
                                      int commandId );                 // mainfrm.cpp:1340

// ── ROUND AN (deferred) — the PATCH SOURCE arm ──────────────────────────────
// The post-control-point-edit bookkeeping, and the map-dirty flag that goes with
// it.  BOTH declarations are copied verbatim from kiwi_transform.cpp:57 / :58,
// which is the file that already moves patch control points (ApplyVerts) — one
// spelling of the pair in the whole UX layer, so the patch arm here and the patch
// arm of the move gizmo cannot drift about what "rebuild" means.
extern void  Patch_Rebuild( patchMesh_t *p, char doBounds );           // pmesh.cpp:2137 (0x438D80)
extern void  MarkMapModified();                                        // win_qe3.cpp:195  (0x499BB0)

// brush.cpp // KIWI-UX forwarders for the two file-static texture-lock halves
// (the same pair kiwi_transform.cpp uses — brush.cpp:7624 / brush.cpp:7629).
extern void  Ed_FaceTexLockSave( float *saveBuf, face_t *face );
extern void  Ed_FaceTexLockReproject( face_t *face, const float *saveBuf, const byte *lockFlags );

namespace
{
    const float KMATCH_EPS = 1.0e-4f;
    const float KMATCH_SRC_COL  [3] = { 0.85f, 0.45f, 1.00f };   // the source face's boundary
    const float KMATCH_GHOST_COL[3] = { 0.55f, 1.00f, 0.75f };   // where it would land
    const float KMATCH_LINK_COL [3] = { 0.60f, 0.60f, 0.70f };   // source -> target tie line

    // ═════════════════════════════════════════════════════════════════════════
    //  ROUND AN (deferred) — THE THREE NUMBERS THE PATCH ARM IS DECIDED BY
    // ═════════════════════════════════════════════════════════════════════════
    // Every one of these is a REFUSAL threshold, so each is stated with what it
    // costs to be on the wrong side of it.  See kiwi_matchface.h for the whole
    // derivation; these are the values it argues for.
    //
    // KMATCH_PLANAR_EPS — the most any control point may sit off the net's own
    // best-fit plane and still be called flat, in WORLD UNITS.
    //   * 0.1 is the tolerance this codebase ALREADY uses to call two points the
    //     same point: KPF_MATCH_TOL (kiwi_patchfillet.cpp:74), itself the ported
    //     FindPoint dedup tolerance.  A cap flatter than that is flat as far as
    //     every other consumer in the editor is concerned.
    //   * It is comfortably clear of float round-off: a 4096-unit patch carries
    //     ~4e-3 of it through a dot/cross chain, forty times under the limit, so
    //     an honestly flat cap is never refused for arithmetic reasons.
    //   * It is BELOW the finest editor grid step, so a cap bent by even one grid
    //     quantum is refused out loud rather than silently flattened onto a plane
    //     it was never on.
    const float KMATCH_PLANAR_EPS = 0.1f;

    // KMATCH_MIN_AREA2 — the Newell sum's length floor.  That sum is 2*Area for a
    // planar net (see PatchPlane), so this is "the control net encloses more than
    // 5e-4 square units".  It is a DEGENERACY test, not a size preference: below
    // it the normal is pure round-off and every number derived from it is noise.
    const float KMATCH_MIN_AREA2 = 1.0e-3f;

    // KMATCH_MIN_COS — |dot( patchNormal, targetPlaneNormal )|.  The projection
    // travels t = (planeDist - P·N) / (n·N), so this denominator IS the
    // amplification: at 0.1 a point one unit off the plane slides TEN units to
    // reach it, and at zero the patch normal is parallel to the plane and the
    // solve has no answer at all.  10x is already the outer edge of "the cap moved
    // onto that face" still meaning anything; past it a single grazing pick would
    // fling the cap hundreds of units sideways, which is the exact failure a
    // refusal exists to prevent.  Note it is an ABSOLUTE value: the sign of the
    // patch normal is irrelevant to the solve (flip n and both numerator and
    // denominator flip with it), so there is nothing to orient here.
    const float KMATCH_MIN_COS = 0.1f;


    // ── ROUND AA, ITEM 4: "is this face's plane now somebody else's plane?" ──
    // The V5 predicate, spelled with the V5 thresholds (kiwi_validity.h) so the
    // question this file asks and the question the gate asks can never drift
    // apart.  Returns the index of the first other face sharing `faceIndex`'s
    // half-space, or -1.
    int CoincidentFace( const brush_t *def, int faceIndex )
    {
        if ( !def || !def->faces || faceIndex < 0 || faceIndex >= def->faceCount )
            return -1;
        const plane_t &pf = def->faces[faceIndex].plane;
        for ( int i = 0; i < def->faceCount; ++i )
        {
            if ( i == faceIndex )
                continue;
            const plane_t &pi = def->faces[i].plane;
            if ( Dot3( pf.normal, pi.normal ) > KVALID_PLANE_DOT
              && fabsf( pf.dist - pi.dist ) < KVALID_PLANE_DIST )
                return i;
        }
        return -1;
    }

    winding_t *WindingOf( const selbrush_t *b, int faceIndex )
    {
        if ( !b || !b->def || !b->def->faces )
            return 0;
        if ( faceIndex < 0 || faceIndex >= b->def->faceCount )
            return 0;
        return b->def->faces[faceIndex].w;
    }

    bool WindingCentre( const winding_t *w, float *out, float *outRadius )
    {
        if ( !w || w->numpoints < 3 || w->numpoints > MAX_POINTS_ON_WINDING )
            return false;
        out[0] = out[1] = out[2] = 0.0f;
        for ( int i = 0; i < w->numpoints; ++i )
            for ( int k = 0; k < 3; ++k )
                out[k] += w->p[i][k];
        const float inv = 1.0f / (float)w->numpoints;
        for ( int k = 0; k < 3; ++k )
            out[k] *= inv;

        float r = 0.0f;
        for ( int i = 0; i < w->numpoints; ++i )
        {
            float rel[3];
            Sub3( w->p[i], out, rel );
            const float d = Len3( rel );
            if ( d > r )
                r = d;
        }
        if ( outRadius )
            *outRadius = ( r > 1.0f ) ? r : 16.0f;
        return true;
    }

    // ═════════════════════════════════════════════════════════════════════════
    //  ROUND AN (deferred) — THE PATCH SOURCE: ITS MESH, ITS PLANE, ITS SOLVE
    // ═════════════════════════════════════════════════════════════════════════

    // The patch DEF behind a selection node, with the same guards kiwi_patchverts
    // .cpp's PatchOf uses (pmesh.cpp:3851 asserts the instance and def spellings
    // agree; the DEF's is the one every ported patch consumer reads).
    patchMesh_t *PatchMeshOf( selbrush_t *b )
    {
        if ( !Sel_BrushLive( b ) || !b->patch || !b->def )
            return 0;
        patchMesh_t *pm = b->def->patch;
        if ( !pm || pm->width < 2 || pm->height < 2 || pm->width > 16 || pm->height > 16 )
            return 0;
        return pm;
    }

    // ── THE PLANE FIT: NEWELL, PER CELL OF THE CONTROL NET ───────────────────
    // WHY NEWELL AND NOT A CROSS PRODUCT OF TWO SPANNING EDGES.  The obvious
    // cheap fit — cross( ctrl[w-1][0] - ctrl[0][0], ctrl[0][h-1] - ctrl[0][0] ) —
    // is DEGENERATE ON THE VERY PATCH THIS FEATURE EXISTS FOR.  A fillet end cap
    // is built by kiwi_patchfillet.cpp's LandCaps as an arc profile on one row and
    // its projection onto the chamfer plane on the other, and the arc is TANGENT
    // to that plane at both ends — so at col 0 and col w-1 the point and its
    // projection are literally the same point (kiwi_patchfillet.cpp:1676-1679 says
    // so in as many words, and picks the middle column for exactly this reason).
    // The second spanning edge is therefore the zero vector and the cross product
    // is nothing.  Any fix that "picks a better pair of columns" is a special case
    // waiting to be wrong on the next patch shape.
    //
    // Newell's method has no such corner: it sums a signed area contribution over
    // EVERY edge of every cell, so no single degenerate edge can decide the
    // answer, and for a planar net the sum is exactly 2*Area*n — the true normal,
    // not an approximation, whatever the outline's shape or convexity.  Cells are
    // walked in the same (col,row) parametric order throughout, so a planar net's
    // contributions all point the same way and add rather than cancel.
    //
    // AND IT IS THE CONTROL NET THAT IS FITTED, not the tessellation, which is the
    // point of the whole test: a bezier surface lies in the convex hull of its
    // control points, so control points in a plane means SURFACE in that plane.
    // "Planar control net" and "flat patch" are therefore the same statement, and
    // projecting the control points is exactly projecting the surface.
    //
    // `outCentre` is the control-point centroid — the point every least-squares
    // plane passes through, and the anchor the deviation below is measured from.
    // `outDev` is max |signed distance| of any control point to that plane, i.e.
    // the planarity residual the caller gates on.
    bool PatchPlane( const patchMesh_t *pm, float *outN, float *outCentre, float *outDev )
    {
        if ( !pm || pm->width < 2 || pm->height < 2 || pm->width > 16 || pm->height > 16 )
            return false;

        float sum[3] = { 0.0f, 0.0f, 0.0f };
        for ( int col = 0; col + 1 < pm->width; ++col )
            for ( int row = 0; row + 1 < pm->height; ++row )
            {
                const float *q[4] = { pm->ctrl[col    ][row    ].xyz,
                                      pm->ctrl[col + 1][row    ].xyz,
                                      pm->ctrl[col + 1][row + 1].xyz,
                                      pm->ctrl[col    ][row + 1].xyz };
                for ( int e = 0; e < 4; ++e )
                {
                    const float *a = q[e];
                    const float *b = q[( e + 1 ) & 3];
                    sum[0] += ( a[1] - b[1] ) * ( a[2] + b[2] );
                    sum[1] += ( a[2] - b[2] ) * ( a[0] + b[0] );
                    sum[2] += ( a[0] - b[0] ) * ( a[1] + b[1] );
                }
            }

        // |sum| is 2*Area.  Below the floor the direction is round-off (see
        // KMATCH_MIN_AREA2), so refuse BEFORE normalising rather than normalising
        // noise and letting the caller act on it.
        if ( !( Len3( sum ) > KMATCH_MIN_AREA2 ) )
            return false;
        Copy3( sum, outN );
        if ( !Norm3( outN ) )
            return false;

        float c[3] = { 0.0f, 0.0f, 0.0f };
        for ( int col = 0; col < pm->width; ++col )
            for ( int row = 0; row < pm->height; ++row )
                for ( int k = 0; k < 3; ++k )
                    c[k] += pm->ctrl[col][row].xyz[k];
        const float inv = 1.0f / (float)( pm->width * pm->height );
        for ( int k = 0; k < 3; ++k )
            c[k] *= inv;
        Copy3( c, outCentre );

        float dev = 0.0f;
        for ( int col = 0; col < pm->width; ++col )
            for ( int row = 0; row < pm->height; ++row )
            {
                float rel[3];
                Sub3( pm->ctrl[col][row].xyz, c, rel );
                const float s = fabsf( Dot3( rel, outN ) );
                if ( s > dev )
                    dev = s;
            }
        if ( outDev )
            *outDev = dev;
        return true;
    }

    // P -> P + n*t with t solving ( P + n*t ) . planeN = planeD.  `denom` is
    // n.planeN, hoisted out because the caller has already gated on it.
    inline void ProjectAlong( const float *p, const float *n, const float *planeN,
                              float planeD, float denom, float *out )
    {
        const float t = ( planeD - Dot3( planeN, p ) ) / denom;
        Mad3( p, n, t, out );
    }

    // KIWI-UX (CLEANUP, UndoCoverBrush): the local copy is gone — this was one
    // of five verbatim bodies.  It is KiwiCmd_UndoCoverBrush (kiwi_command.h)
    // now, beside the bracket whose blind spot it exists to fill.

    // ═════════════════════════════════════════════════════════════════════════
    //  Z — MATCH FACE.
    // ═════════════════════════════════════════════════════════════════════════
    class KiwiMatchCommand : public KiwiEditorCommand
    {
    public:
        const char *Name() const override { return "Match Face"; }
        bool CanExecute() override { return KiwiMatch_CanMatch(); }

        // The target is chosen by a CLICK, so this is a multi-click tool: the
        // framework hands the press to Click() instead of pausing the gesture
        // (kiwi_command.h WantsClicks).  It is exactly the drawing tools' grammar
        // — a click here NAMES something rather than ending a drag.
        bool WantsClicks() const override { return true; }

        // No scalar to type.  Leaving the field table empty also leaves Tab free,
        // which this command does not use but costs nothing to keep consistent
        // with the other shakeout-G verbs.
        int NumericFields( const kiwiNumField_t **out ) const override
        { (void)out; return 0; }

        const char *HudStatus() const override { return m_hud[0] ? m_hud : 0; }
        bool        HudInvalid() const override { return m_rejected; }

        bool Begin() override
        {
            m_haveTarget = false;
            m_rejected   = false;
            m_srcPatch   = 0;               // ROUND AN: which arm is running.  Cleared
                                            //   FIRST, so a failed start can never
                                            //   leave the previous gesture's answer
                                            //   standing for the next one.
            m_node       = 0;
            m_face       = -1;
            m_hud[0]     = '\0';

            const selection_t &sel = KiwiSel();
            const sel_item_t  *src = 0;
            int                n   = 0;
            for ( size_t i = 0; i < sel.items.size(); ++i )
            {
                if ( sel.items[i].kind != SEL_FACE || !Sel_BrushLive( sel.items[i].brush ) )
                    continue;
                src = &sel.items[i];
                ++n;
            }

            // ── ROUND AN (deferred): NO FACE SELECTED — TRY THE PATCH ARM ────
            // The FACE arm keeps first refusal, and deliberately: with a face
            // selected the verb means what it has always meant, whatever else
            // happens to be selected alongside it.  Only when the selection names
            // no face at all does the question become "is this a flat curve?".
            if ( n == 0 )
                return BeginPatchSource();

            if ( n != 1 || !src || src->brush->patch )
            {
                Sys_Printf( "Match Face: select exactly ONE brush face first.\n" );
                return false;
            }

            m_node = src->brush;
            m_face = src->faceIndex;
            brush_t *def = m_node->def;
            if ( !def || !def->faces || m_face < 0 || m_face >= def->faceCount
              || !def->faces[m_face].w )
            {
                Sys_Printf( "Match Face: that face has no winding.\n" );
                return false;
            }

            // The baseline the trial rolls back to, and the texdef half of it —
            // KiwiValid_Snapshot only knows planepts / control points.
            if ( !KiwiValid_Snapshot( def, &m_base ) )
                return false;
            memcpy( m_baseMtl, def->faces[m_face].mtldef, sizeof( m_baseMtl ) );   // CLEANUP, B-35
            Copy3( def->faces[m_face].plane.normal, m_srcNormal );

            UpdateHud();
            Sys_Printf( "Match Face: click the face to copy the plane FROM "
                        "(Esc cancels).\n" );
            return true;
        }

        // THE FRAMEWORK'S PICK IS NOT USABLE HERE, and the reason is worth stating
        // once: KiwiCmd_MouseMove builds it with `KiwiSel_GetModeMask()` — the
        // user's CURRENT SELECTION MODE (kiwi_command.cpp).  Match Face is started
        // from a face selection but the mode may well be Object or All, in which
        // case that pick would resolve to a whole brush and this command would
        // never see a face at all.  So it re-casts its own ray under a FACE-ONLY
        // mask; the framework's pick is ignored (and its snap with it — there is
        // nothing here for a snap point to mean).
        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick; (void)snap;
            m_haveTarget = false;

            int   x, y;
            ray_t ray;
            if ( KiwiCmd_LastCursor( &x, &y ) && Pick_RayFromImagePos( x, y, &ray ) )
            {
                const pick_result_t hit = Pick( ray, SEL_MASK_FACE );
                if ( hit.valid && hit.item.kind == SEL_FACE && Sel_BrushLive( hit.item.brush )
                  && !hit.item.brush->patch
                  && !( hit.item.brush == m_node && hit.item.faceIndex == m_face ) )
                {
                    m_target     = hit.item;
                    m_haveTarget = BuildPlanePts( m_target, m_pts, m_ghostN );
                }
            }
            m_rejected = false;
            UpdateHud();
            g_nUpdateBits |= 1;
        }

        // Return false = COMMIT (kiwi_command.h Click).  True keeps the gesture
        // running, which is what a click on nothing, or a rejected match, does.
        bool Click() override
        {
            if ( !m_haveTarget )
            {
                Sys_Printf( "Match Face: that is not a face — click the face to "
                            "match, or press Esc.\n" );
                return true;
            }
            if ( !Sel_BrushLive( m_node ) )
            {
                Sys_Printf( "Match Face: the source brush went away.\n" );
                return false;
            }

            // ── ROUND AN (deferred): THE PATCH ARM OWNS THE CLICK ────────────
            // Placed after the two guards above because they are the SAME two
            // questions for both arms (is there a target, is the source still
            // there) and asking them twice would be two places to get them wrong.
            if ( m_srcPatch )
                return ClickPatch();

            // ── the TRIAL (kiwi_matchface.h): no bracket, no texture lock ─────
            WritePlanePts( m_pts );
            KiwiValid_Rebuild( m_node->def );

            // ═══════════════════════════════════════════════════════════════════
            //  ROUND AA, ITEM 4 — THE PLANARIZE-AWAY CASE
            // ═══════════════════════════════════════════════════════════════════
            // USER REPORT, verbatim: "(see pic) match face should support this
            // operation.  You still can't delete a chamfer that was made on a
            // brush."  The picture is a SKEWED chamfer face with a neighbouring
            // face outlined as the target.
            //
            // WHY IT WAS REFUSED, exactly.  The chamfer face taking its
            // neighbour's plane VERBATIM is the whole point of the verb, and it
            // works: BuildPlanePts puts the three planepts on the target's plane
            // and Face_MakePlane reproduces the target's normal and dist to float
            // precision (kiwi_matchface.h derives that).  The two faces are then
            // COPLANAR — and V5 rejected it:
            //
            //     kiwi_validity.cpp:280  if ( d > KVALID_PLANE_DOT
            //                              && fabsf( pi.dist - pj.dist ) < KVALID_PLANE_DIST )
            //                                { *outWhy = "duplicate plane"; return false; }
            //
            // reached from the trial below, so the operation reported "rejected —
            // duplicate plane" and changed nothing.  Nothing about the SOURCE face
            // being skewed or non-axial was ever the problem; there is no axiality
            // assumption anywhere in this file.
            //
            // V5 IS NOT WEAKENED, because V5 is right: a brush written to disk
            // carrying the same half-space twice is ambiguous to the compiler.
            // What was wrong was gating the INTERMEDIATE rather than the RESULT.
            // A half-space that duplicates another clips nothing its twin did not
            // already clip, so the brush the user asked for is this same geometry
            // with the now-redundant face DROPPED — which is a perfectly ordinary
            // solid, and is precisely "delete the chamfer" arriving from the other
            // direction.  So: detect the twin, gate the brush MINUS the redundant
            // face (KiwiValid_CheckBrushIgnoringFace), and add V8 on top because
            // this is one of the two verbs that can take a bounding plane away.
            //
            // THE REDUNDANT FACE IS ALWAYS THE SOURCE, never the twin: every other
            // face was in the brush before this gesture and is not ours to remove.
            const int twin = CoincidentFace( m_node->def, m_face );

            const char *why = "invalid geometry";
            const bool  ok  = KiwiValid_CheckBrushIgnoringFace(
                                  m_node->def, twin >= 0 ? m_face : -1, &why )
                           && KiwiValid_BrushCloses( m_node->def, &why );
            KiwiValid_Restore( m_base );            // back to the ORIGINAL, either way

            if ( !ok )
            {
                m_rejected = true;
                UpdateHud();
                Sys_Printf( "Match Face: rejected — %s.  Nothing was changed.\n",
                            why ? why : "invalid geometry" );
                g_nUpdateBits = -1;
                return true;                        // stay running: pick another face
            }

            // ── the real apply: bracket, then the §20 texture-lock sequence ──
            KiwiCmd_UndoBegin( "match face" );      // clones the untouched original
            KiwiCmd_UndoCoverBrush( m_node );               // …a FACE selection is not on
                                                    //   selected_brushes, so the
                                                    //   bracket head covered nothing

            // ── ROUND AA, ITEM 4: the planarize-away apply ────────────────────
            // ONE Brush_RemoveFace and a rebuild, and NOT "write the plane, then
            // remove the face".  The trial rolled the brush back to the baseline,
            // so the source face still carries its ORIGINAL (chamfer) plane here —
            // and removing it now gives byte-for-byte the solid the trial gated,
            // because the plane we were going to write was a duplicate of `twin`'s
            // and a duplicated half-space clips nothing its twin did not already
            // clip.  Writing it first would only be two rebuilds for one result.
            //
            // EVERY SURVIVING FACE KEEPS ITS MATERIAL AND ITS TEXDEF, and neither
            // is re-derived: Brush_RemoveFace (brush.cpp:334) memmoves whole
            // 232-byte face_t records down one slot, so each surviving plane keeps
            // the planepts, all four MaterialDef layers, the contents and the
            // toolflags it arrived with.  There is no texture lock to run either —
            // lock exists to re-project a texdef when its own PLANE moves, and not
            // one surviving plane moves here.  The neighbours only get BIGGER
            // windings (they re-extend to the edge the chamfer had cut off), which
            // is a winding change, not a plane change, and a texdef is anchored to
            // the plane.
            if ( twin >= 0 )
            {
                Brush_RemoveFace( m_node->def, (unsigned int)m_face );
                KiwiValid_Rebuild( m_node->def );

                // Brush_RemoveFace shifted every later face down a slot, so every
                // (faceIndex, edgeIndex) still held anywhere names a DIFFERENT
                // surface now — the same staleness kiwi_bevel.cpp's Remove Face
                // drops its selection for.  m_face goes with it so DrawWorld and
                // Commit cannot resolve it either.
                m_face = -1;
                Sel_Clear( KiwiSel() );
                Sel_RebuildFromLegacy();

                Sys_Printf( "Match Face: the face is now coplanar with its "
                            "neighbour — the redundant plane was removed and the "
                            "edge restored (%i face(s) left).\n",
                            m_node->def->faceCount );
                g_nUpdateBits = -1;
                return false;                       // COMMIT
            }

            byte lockFlags[3];
            lockFlags[0] = (byte)( g_PrefsDlg->m_bTextureLock  != 0 );
            lockFlags[1] = (byte)( g_PrefsDlg->m_bLightmapLock != 0 );
            lockFlags[2] = 1;
            float saveBuf[19];

            face_t *f = &m_node->def->faces[m_face];
            memcpy( f->mtldef, m_baseMtl, sizeof( m_baseMtl ) );                   // CLEANUP, B-35
            Face_MakePlane( f );                    // the plane must describe the
                                                    // BASELINE planepts before the save
            if ( f->w )
                Ed_FaceTexLockSave( saveBuf, f );

            WritePlanePts( m_pts );

            if ( f->w )
                Ed_FaceTexLockReproject( f, saveBuf, lockFlags );

            KiwiValid_Rebuild( m_node->def );
            Sys_Printf( "Match Face: plane copied.\n" );
            g_nUpdateBits = -1;
            return false;                           // COMMIT
        }

        void Commit() override
        {
            m_haveTarget = false;
            g_nUpdateBits = -1;
        }

        void Cancel() override
        {
            // Nothing to restore: the trial always rolls itself back, and the real
            // apply only ever runs on the path that immediately commits.  The
            // bracket helpers self-guard when nothing was opened.
            m_haveTarget = false;
            g_nUpdateBits |= 1;
        }

        // PREVIEW, and exactly what it is: the SOURCE face's boundary in violet
        // (the fill that tints its interior is the ordinary §18 selected-face fill
        // — kiwi_hover.cpp — so nothing is drawn twice), a GHOST loop where that
        // boundary would land once the plane is copied, and a tie line between the
        // two face centres so the pairing is unmistakable on a crowded brush.
        //
        // The ghost is the source winding projected onto the target plane ALONG
        // THE SOURCE NORMAL.  It is a best-effort figure, not the real result:
        // the real face is re-clipped against the brush's other planes by
        // Brush_BuildWindings and can gain or lose vertices.  It answers the
        // question the user actually has — "where does this face end up" — and
        // that is what it is labelled as.
        void DrawWorld() override
        {
            // ROUND AN (deferred): the patch arm has no source WINDING to draw —
            // it draws the control net and where that net would land instead.
            if ( m_srcPatch )
            {
                DrawPatchPreview();
                return;
            }

            winding_t *sw = WindingOf( m_node, m_face );
            if ( !sw || sw->numpoints < 3 || sw->numpoints > MAX_POINTS_ON_WINDING )
                return;

            KiwiLines_Color( KMATCH_SRC_COL[0], KMATCH_SRC_COL[1], KMATCH_SRC_COL[2] );
            for ( int i = 0; i < sw->numpoints; ++i )
                if ( !KiwiLines_Add( sw->p[i], sw->p[( i + 1 ) % sw->numpoints] ) )
                    return;

            if ( !m_haveTarget )
                return;

            const float d = Dot3( m_ghostN, m_pts[1] );      // pts[1] is the plane point
            const float denom = Dot3( m_ghostN, m_srcNormal );
            if ( fabsf( denom ) < KMATCH_EPS )
                return;                                       // source is edge-on to the
                                                              // target plane: no projection

            KiwiLines_Color( KMATCH_GHOST_COL[0], KMATCH_GHOST_COL[1], KMATCH_GHOST_COL[2] );
            float prev[3];
            float first[3];
            for ( int i = 0; i < sw->numpoints; ++i )
            {
                float p[3];
                const float t = ( d - Dot3( m_ghostN, sw->p[i] ) ) / denom;
                Mad3( sw->p[i], m_srcNormal, t, p );
                if ( i == 0 )
                    Copy3( p, first );
                else if ( !KiwiLines_Add( prev, p ) )
                    return;
                Copy3( p, prev );
            }
            KiwiLines_Add( prev, first );

            float sc[3], tc[3], r;
            winding_t *tw = WindingOf( m_target.brush, m_target.faceIndex );
            if ( WindingCentre( sw, sc, &r ) && WindingCentre( tw, tc, &r ) )
            {
                KiwiLines_Color( KMATCH_LINK_COL[0], KMATCH_LINK_COL[1], KMATCH_LINK_COL[2] );
                KiwiLines_Add( sc, tc );
            }
        }

    private:
        // ═══════════════════════════════════════════════════════════════════
        //  ROUND AN (deferred) — Z ON A CURVE: PUSH ITS CONTROL POINTS TO A FACE
        // ═══════════════════════════════════════════════════════════════════
        // USER DIRECTIVE, verbatim: "the cap needs to be adjustable, I would use
        // the (Z) match face command on the curve itself."  The full argument —
        // why control points and not the tessellation, why along the PATCH's own
        // normal, and why a bent curve is refused rather than approximated — is in
        // kiwi_matchface.h.  This is the source stage: latch the patch, fit its
        // plane, and refuse now if it is not flat, because refusing at Begin costs
        // the user one keystroke and refusing at the click costs them a pick.
        bool BeginPatchSource()
        {
            const selection_t &sel  = KiwiSel();
            selbrush_t        *node = 0;
            int                n    = 0;
            for ( size_t i = 0; i < sel.items.size(); ++i )
            {
                const sel_item_t &it = sel.items[i];
                // A patch is named by an OBJECT selection — it has no faces of its
                // own to select, and its control points are SEL_VERTEX items that
                // belong to the V mode (kiwi_patchverts.h), not to this verb.
                if ( it.kind != SEL_OBJECT || !PatchMeshOf( it.brush ) )
                    continue;
                node = it.brush;
                ++n;
            }
            if ( n != 1 || !node )
            {
                Sys_Printf( "Match Face: select exactly ONE brush face — or ONE flat "
                            "curve, to push it onto a face.\n" );
                return false;
            }

            patchMesh_t *pm  = PatchMeshOf( node );
            float        dev = 0.0f;
            if ( !PatchPlane( pm, m_srcNormal, m_srcCentre, &dev ) )
            {
                Sys_Printf( "Match Face: that curve has no usable plane — its control "
                            "net encloses no area.\n" );
                return false;
            }
            if ( dev > KMATCH_PLANAR_EPS )
            {
                // THE REFUSAL IS NAMED, AND SO IS THE NUMBER.  A tool that says only
                // "no" teaches nothing; one that says how far out of flat the curve
                // is, and by what limit, tells the user whether they are one nudge
                // away or asking for something the verb cannot mean.
                Sys_Printf( "Match Face: that curve is not planar — only a flat cap "
                            "can be matched to a face.  (It bends %.4g unit(s) out of "
                            "its own best-fit plane; the limit is %g.)\n",
                            (double)dev, (double)KMATCH_PLANAR_EPS );
                return false;
            }

            m_node     = node;
            m_face     = -1;                // no face index in this arm, ever
            m_srcPatch = pm;
            UpdateHud();
            Sys_Printf( "Match Face: flat curve (%ix%i control points).  Click the face "
                        "to push it onto — it slides along its OWN normal, so a cap "
                        "stays the same shape and only moves (Esc cancels).\n",
                        pm->width, pm->height );
            return true;
        }

        // ── THE APPLY, AND WHY THERE IS NO TRIAL HERE ───────────────────────
        // The FACE arm has to mutate to find out (kiwi_matchface.h "WHY THE APPLY
        // RUNS TWICE"): a copied plane re-clips the whole solid against every other
        // half-space, and only Brush_BuildWindings can say whether what comes out
        // is still a brush.  A PATCH HAS NO SUCH QUESTION.  It is not clipped
        // against anything — it IS its control points — so every reason to refuse
        // can be answered on a scratch copy with nothing touched.  Hence the shape
        // below: solve all 256 points into `moved`, gate the RESULT, and only then
        // open the bracket.  A rejected pick therefore costs nothing at all, which
        // is the same promise the trial buys the face arm by a longer road.
        bool ClickPatch()
        {
            patchMesh_t *pm = PatchMeshOf( m_node );
            if ( !pm || pm != m_srcPatch )
            {
                Sys_Printf( "Match Face: the curve went away.\n" );
                return false;                       // COMMIT — nothing was changed
            }

            // Re-derive rather than trust the latch: the gesture is modal but not
            // exclusive, and a curve that has been reshaped since Begin must be
            // re-asked the planarity question, not projected on an old answer.
            float n[3], c[3], dev = 0.0f;
            if ( !PatchPlane( pm, n, c, &dev ) || dev > KMATCH_PLANAR_EPS )
            {
                Sys_Printf( "Match Face: that curve is not planar — only a flat cap "
                            "can be matched to a face.  Nothing was changed.\n" );
                return false;                       // a SOURCE fault: re-picking a
                                                    //   target cannot help, so end
            }
            Copy3( n, m_srcNormal );
            Copy3( c, m_srcCentre );

            // The target plane, in the same two numbers DrawWorld's ghost uses:
            // m_ghostN is BuildPlanePts' unit normal and m_pts[1] is a point on it.
            const float planeD = Dot3( m_ghostN, m_pts[1] );
            const float denom  = Dot3( n, m_ghostN );
            if ( fabsf( denom ) < KMATCH_MIN_COS )
            {
                m_rejected = true;
                UpdateHud();
                Sys_Printf( "Match Face: rejected — the curve is edge-on to that face "
                            "(%.3g, under %g), so sliding it there would take it "
                            "%.0fx its own distance away.  Pick a face the curve "
                            "faces.\n", (double)fabsf( denom ), (double)KMATCH_MIN_COS,
                            (double)( 1.0f / KMATCH_MIN_COS ) );
                g_nUpdateBits = -1;
                return true;                        // a TARGET fault: stay running
            }

            // ── THE SCRATCH SOLVE ────────────────────────────────────────────
            float moved[16][16][3];
            float maxT = 0.0f;
            for ( int col = 0; col < pm->width; ++col )
                for ( int row = 0; row < pm->height; ++row )
                {
                    const float *p = pm->ctrl[col][row].xyz;
                    ProjectAlong( p, n, m_ghostN, planeD, denom, moved[col][row] );

                    float rel[3];
                    Sub3( moved[col][row], p, rel );
                    const float t = Len3( rel );
                    if ( t > maxT )
                        maxT = t;
                    for ( int k = 0; k < 3; ++k )
                        if ( fabsf( moved[col][row][k] ) > KVALID_MAX_COORD )
                        {
                            m_rejected = true;
                            UpdateHud();
                            Sys_Printf( "Match Face: rejected — that projection puts "
                                        "the curve outside the map.  Nothing was "
                                        "changed.\n" );
                            g_nUpdateBits = -1;
                            return true;
                        }
                }
            // §19's V7 span, applied to the TRAVEL rather than to the result: the
            // cosine gate bounds the amplification but not the distance, and a
            // target plane a map away is still a legal plane.  KVALID_MAX_SPAN is
            // the map bound the gate already uses (kiwi_validity.h).
            if ( maxT > KVALID_MAX_SPAN )
            {
                m_rejected = true;
                UpdateHud();
                Sys_Printf( "Match Face: rejected — that face is %.0f units away along "
                            "the curve's normal, past the %g map span.  Nothing was "
                            "changed.\n", (double)maxT, (double)KVALID_MAX_SPAN );
                g_nUpdateBits = -1;
                return true;
            }

            // ── ONE RECORD, ONE GESTURE ──────────────────────────────────────
            // The bracket head is the file's own (KiwiCmd_UndoBegin clones every
            // brush on selected_brushes), and UndoCoverBrush on top of it is the
            // same belt kiwi_transform.cpp's ApplyVerts wears for its patch arm.
            // It is NOT a double-save: Undo_AddBrush returns early when the brush
            // is already in the record (undo.cpp:502, Undo_BrushInUndo), so the
            // cover is free when the object selection already put it there and
            // load-bearing when — as after a mode change — it did not.
            KiwiCmd_UndoBegin( "match face" );
            KiwiCmd_UndoCoverBrush( m_node );

            for ( int col = 0; col < pm->width; ++col )
                for ( int row = 0; row < pm->height; ++row )
                    Copy3( moved[col][row], pm->ctrl[col][row].xyz );

            // THE PORTED POST-EDIT BOOKKEEPING, and the same call the move gizmo
            // makes after it drags control points (kiwi_transform.cpp:2823):
            // Patch_Rebuild( def, 1 ) is what Patch_UpdateSelected_0 (pmesh.cpp
            // 0x43D800) runs after translating queued points — bounds recompute →
            // Brush_RebuildBrush → curveDef re-tessellate → ++version.  ONE call
            // for the whole net, because it re-tessellates the entire mesh.
            Patch_Rebuild( pm, 1 );
            MarkMapModified();

            Sys_Printf( "Match Face: curve pushed onto the face (%ix%i control points, "
                        "%.4g unit(s) at most).\n", pm->width, pm->height, (double)maxT );
            g_nUpdateBits = -1;
            return false;                           // COMMIT
        }

        // The patch arm's preview, in the face arm's own three colours so the two
        // read as one verb: the control net where it is, the net where it would
        // land, and the tie line that pairs source with target.
        void DrawPatchPreview()
        {
            patchMesh_t *pm = PatchMeshOf( m_node );
            if ( !pm || pm != m_srcPatch )
                return;

            KiwiLines_Color( KMATCH_SRC_COL[0], KMATCH_SRC_COL[1], KMATCH_SRC_COL[2] );
            for ( int col = 0; col < pm->width; ++col )
                for ( int row = 0; row < pm->height; ++row )
                {
                    if ( col + 1 < pm->width
                      && !KiwiLines_Add( pm->ctrl[col][row].xyz, pm->ctrl[col + 1][row].xyz ) )
                        return;
                    if ( row + 1 < pm->height
                      && !KiwiLines_Add( pm->ctrl[col][row].xyz, pm->ctrl[col][row + 1].xyz ) )
                        return;
                }

            if ( !m_haveTarget )
                return;

            const float planeD = Dot3( m_ghostN, m_pts[1] );
            const float denom  = Dot3( m_srcNormal, m_ghostN );
            if ( fabsf( denom ) < KMATCH_MIN_COS )
                return;                     // the click would refuse this; so does
                                            //   the preview, rather than drawing a
                                            //   ghost the user cannot have

            float g[16][16][3];
            for ( int col = 0; col < pm->width; ++col )
                for ( int row = 0; row < pm->height; ++row )
                    ProjectAlong( pm->ctrl[col][row].xyz, m_srcNormal, m_ghostN,
                                  planeD, denom, g[col][row] );

            KiwiLines_Color( KMATCH_GHOST_COL[0], KMATCH_GHOST_COL[1], KMATCH_GHOST_COL[2] );
            for ( int col = 0; col < pm->width; ++col )
                for ( int row = 0; row < pm->height; ++row )
                {
                    if ( col + 1 < pm->width && !KiwiLines_Add( g[col][row], g[col + 1][row] ) )
                        return;
                    if ( row + 1 < pm->height && !KiwiLines_Add( g[col][row], g[col][row + 1] ) )
                        return;
                }

            float tc[3], r;
            winding_t *tw = WindingOf( m_target.brush, m_target.faceIndex );
            if ( WindingCentre( tw, tc, &r ) )
            {
                KiwiLines_Color( KMATCH_LINK_COL[0], KMATCH_LINK_COL[1], KMATCH_LINK_COL[2] );
                KiwiLines_Add( m_srcCentre, tc );
            }
        }

        // The three well-spread planepts on `target`'s plane, oriented so that
        // Face_MakePlane reproduces the SOURCE's outward sense (kiwi_matchface.h).
        bool BuildPlanePts( const sel_item_t &target, float pts[3][3], float outN[3] ) const
        {
            winding_t *w = WindingOf( target.brush, target.faceIndex );
            if ( !w )
                return false;
            float c[3], r;
            if ( !WindingCentre( w, c, &r ) )
                return false;

            float n[3];
            Copy3( target.brush->def->faces[target.faceIndex].plane.normal, n );
            if ( !Norm3( n ) )
                return false;
            // Keep the source's outward sense — see the header.
            if ( Dot3( n, m_srcNormal ) < 0.0f )
            {
                n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2];
            }

            // A right-handed in-plane basis (u, v, n): seed u from the world axis
            // that is LEAST aligned with n, so the cross product never collapses.
            float seed[3] = { 0.0f, 0.0f, 0.0f };
            {
                int best = 0;
                for ( int k = 1; k < 3; ++k )
                    if ( fabsf( n[k] ) < fabsf( n[best] ) )
                        best = k;
                seed[best] = 1.0f;
            }
            float u[3], v[3];
            Cross3( n, seed, v );
            if ( !Norm3( v ) )
                return false;
            Cross3( v, n, u );
            if ( !Norm3( u ) )
                return false;
            // (u, v, n) is right-handed: u x v == n, which is what makes the point
            // order below reproduce n exactly.

            Mad3( c, u, r, pts[0] );
            Copy3( c, pts[1] );
            Mad3( c, v, r, pts[2] );
            Copy3( n, outN );
            return true;
        }

        void WritePlanePts( const float pts[3][3] )
        {
            face_t *f = &m_node->def->faces[m_face];
            for ( int p = 0; p < 3; ++p )
                Copy3( pts[p], f->planepts[p] );
        }

        void UpdateHud()
        {
            // ROUND AN (deferred): the line NAMES WHICH ARM IS RUNNING.  The two
            // do different things to different geometry, and a strip that said
            // "match face" while the thing about to move was a curve would be the
            // one moment the user needed it to be specific.
            const char *what = m_srcPatch ? "match curve" : "match face";
            if ( m_rejected )
            {
                _snprintf( m_hud, sizeof( m_hud ), "%s  REJECTED - pick another", what );
            }
            else if ( m_haveTarget )
            {
                _snprintf( m_hud, sizeof( m_hud ), "%s  target under cursor - click to apply", what );
            }
            else if ( m_srcPatch )
            {
                _snprintf( m_hud, sizeof( m_hud ), "%s  hover the face to push it ONTO", what );
            }
            else
            {
                _snprintf( m_hud, sizeof( m_hud ), "%s  hover the face to copy FROM", what );
            }
            m_hud[sizeof( m_hud ) - 1] = '\0';
        }

        selbrush_t     *m_node = 0;
        int             m_face = -1;
        // ROUND AN (deferred): non-null = the PATCH arm is running, and this is the
        // mesh it latched.  It doubles as the arm selector everywhere (Click,
        // DrawWorld, the HUD) precisely so there is ONE fact deciding it, and as a
        // liveness check — PatchMeshOf is re-read and compared against it before
        // anything is drawn or written, so a patch that was deleted, replaced or
        // reallocated under the gesture is caught rather than followed.
        patchMesh_t    *m_srcPatch = 0;
        float           m_srcCentre[3] = { 0.0f, 0.0f, 0.0f };
        sel_item_t      m_target;
        bool            m_haveTarget = false;
        bool            m_rejected   = false;
        float           m_srcNormal[3] = { 0.0f, 0.0f, 1.0f };
        float           m_ghostN[3]    = { 0.0f, 0.0f, 1.0f };
        float           m_pts[3][3]    = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f },
                                           { 0.0f, 0.0f, 0.0f } };
        kiwiBaseBrush_t m_base;
        // KIWI-UX (CLEANUP, B-35): a TYPED array, not a raw byte buffer.  It
        // mirrors face_t::mtldef (`MaterialDef mtldef[4]`, qe3.h:148), so the
        // sizeof() at both memcpy sites is derived from the type rather than
        // restating the element count as a literal 4.
        MaterialDef     m_baseMtl[4];
        char            m_hud[128] = { 0 };
    };

    KiwiMatchCommand s_match;
}

// ─── §3 canExecute ───────────────────────────────────────────────────────────
// ROUND AN (deferred): TWO shapes of selection now start this verb — one face, or
// (with no face named at all) one patch.  Deliberately LOOSER than Begin: the
// palette predicate answers "is this key worth offering", and the planarity fit
// that decides whether the curve can actually be matched is real work that has no
// business running once per palette repaint.  Begin does it, out loud, with the
// reason.  That split is the same one the face arm already has — this predicate
// never checked for a winding either.
bool KiwiMatch_CanMatch()
{
    const selection_t &sel = KiwiSel();
    int faces = 0, patches = 0;
    for ( size_t i = 0; i < sel.items.size(); ++i )
    {
        const sel_item_t &it = sel.items[i];
        // KIWI-UX (CLEANUP, B-5): a stored selbrush_t* is only safe to deref after
        // Sel_BrushLive — same shape as KiwiPatchFillet_CanFillet.
        if ( !Sel_BrushLive( it.brush ) )
            continue;
        if ( it.kind == SEL_FACE )
            ++faces;
        else if ( it.kind == SEL_OBJECT && it.brush->patch )
            ++patches;
    }
    return ( faces == 1 ) || ( faces == 0 && patches == 1 );
}

// ─── registration + lookup ───────────────────────────────────────────────────
void KiwiMatch_RegisterCommands()
{
    Radiant_RegisterCommand( "KiwiMatchFace", 0, 0, KIWI_CMD_MATCH_FACE );
}

KiwiEditorCommand *KiwiMatch_CommandForId( int commandId )
{
    if ( commandId == KIWI_CMD_MATCH_FACE )
        return &s_match;
    return 0;
}

#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Match Face copies a target face plane or projects a flat patch along its normal.
// Plane math and picking are KIWI code; rebuild, validity, and texture lock use ported paths.

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
#include "kiwi_vec.h"     // Dot3/Sub3/etc.

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Ported entry points.
extern int   Sys_Printf( const char *fmt, ... );                       // win_qe3.cpp
extern int   g_nUpdateBits;                                            // 0x25D5A74 (mainfrm.cpp)
extern int   Face_MakePlane( face_t *face );                           // brush.cpp:4495 (0x470470)
// Drops the source when its matched plane duplicates another half-space.
extern size_t Brush_RemoveFace( brush_t *b, unsigned int faceIndex );   // brush.cpp:345  0x471640
// Keep at file scope; a block-scope declaration caused an MSVC linkage mismatch.
extern bool  Radiant_RegisterCommand( const char *name, byte vk, byte mods,
                                      int commandId );                 // mainfrm.cpp:1340

// Patch post-edit bookkeeping; kiwi_transform.cpp uses the same pair.
extern void  Patch_Rebuild( patchMesh_t *p, char doBounds );           // pmesh.cpp:2137 (0x438D80)
extern void  MarkMapModified();                                        // win_qe3.cpp:195  (0x499BB0)

// Forwarders for brush.cpp's file-static texture-lock halves.
extern void  Ed_FaceTexLockSave( float *saveBuf, face_t *face );
extern void  Ed_FaceTexLockReproject( face_t *face, const float *saveBuf, const byte *lockFlags );

namespace
{
    // Avoid a near-zero face-preview projection divisor.
    const float KMATCH_EPS = 1.0e-4f;
    const float KMATCH_SRC_COL  [3] = { 0.85f, 0.45f, 1.00f };   // the source face's boundary
    const float KMATCH_GHOST_COL[3] = { 0.55f, 1.00f, 0.75f };   // where it would land
    const float KMATCH_LINK_COL [3] = { 0.60f, 0.60f, 0.70f };   // source -> target tie line

    // World-unit planarity cutoff; matches the editor's 0.1 point-dedup tolerance.
    const float KMATCH_PLANAR_EPS = 0.1f;

    // For a planar net, Newell length is 2*area; below this, direction is round-off.
    const float KMATCH_MIN_AREA2 = 1.0e-3f;

    // Reject |n·N| below 0.1 because projection amplification would exceed 10x.
    // The absolute value is valid: flipping n also flips the solved scalar t.
    const float KMATCH_MIN_COS = 0.1f;


    // Return the first face sharing faceIndex's half-space under V5 thresholds, or -1.
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
        // Tiny windings still need well-spread plane points.
        if ( outRadius )
            *outRadius = ( r > 1.0f ) ? r : 16.0f;
        return true;
    }

    // Read the DEF mesh used by ported patch consumers, with the 16x16 format bounds.
    patchMesh_t *PatchMeshOf( selbrush_t *b )
    {
        if ( !Sel_BrushLive( b ) || !b->patch || !b->def )
            return 0;
        patchMesh_t *pm = b->def->patch;
        if ( !pm || pm->width < 2 || pm->height < 2 || pm->width > 16 || pm->height > 16 )
            return 0;
        return pm;
    }

    // Newell over every cell avoids the zero spanning edge on fillet caps.
    // A planar Bézier control net keeps the surface planar; outDev is its maximum
    // world-unit residual from the Newell plane through the control-point centroid.
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

        // Refuse a round-off direction before normalization; |sum| is 2*area.
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

    // Match Face modal command.
    class KiwiMatchCommand : public KiwiEditorCommand
    {
    public:
        const char *Name() const override { return "Match Face"; }
        bool CanExecute() override { return KiwiMatch_CanMatch(); }

        // Target selection is a click rather than a drag-ending gesture.
        bool WantsClicks() const override { return true; }

        // No scalar input; Tab remains free.
        int NumericFields( const kiwiNumField_t **out ) const override
        { (void)out; return 0; }

        const char *HudStatus() const override { return m_hud[0] ? m_hud : 0; }
        bool        HudInvalid() const override { return m_rejected; }

        bool Begin() override
        {
            m_haveTarget = false;
            m_rejected   = false;
            m_srcPatch   = 0;               // clear the arm latch before any failure path
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

            // A face selection takes precedence; only zero faces selects the patch arm.
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
            memcpy( m_baseMtl, def->faces[m_face].mtldef, sizeof( m_baseMtl ) );   // snapshot omits materials
            Copy3( def->faces[m_face].plane.normal, m_srcNormal );

            UpdateHud();
            Sys_Printf( "Match Face: click the face to copy the plane FROM "
                        "(Esc cancels).\n" );
            return true;
        }

        // The framework pick uses the current selection-mode mask, which may omit faces.
        // Recast with a face-only mask; snapping has no meaning for this command.
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

        // Click returns false to commit; true keeps a missing/rejected target active.
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

            // Both arms share the target and source-liveness guards above.
            if ( m_srcPatch )
                return ClickPatch();

            // KIWI (2026-09-21, user: "the UV's sometimes get stretched/blown out. all the
            // command should do is a precise extrude"): the plane points written are the
            // face's OWN three points slid along its normal onto the target plane - what
            // push/pull writes (kiwi_transform.cpp ApplyFaces), with a per-point distance.
            // The ported lock keeps the saved S/T AT THE THREE PLANE POINTS, so it is only
            // meaningful when the new points are the old ones moved.  m_pts (the target's
            // centre +/- its radius along an arbitrary basis) are unrelated points, and
            // with Texture Lock on the solve mapped the old texcoords onto them: an
            // arbitrary affine stretch.  "Sometimes" = whenever the lock pref was on.
            float pts[3][3];
            const bool slid = SlidePlanePts( pts );
            if ( !slid )
                memcpy( pts, m_pts, sizeof( pts ) );   // edge-on: plane only, no lock below

            // Trial: no undo bracket or texture lock.
            WritePlanePts( pts );
            KiwiValid_Rebuild( m_node->def );

            // A copied chamfer plane may duplicate an existing V5 half-space.
            // If so, gate with the source omitted and require V8 closure; V5 stays strict.
            // The source is the only face this edit may remove.
            const int twin = CoincidentFace( m_node->def, m_face );

            const char *why = "invalid geometry";
            const bool  ok  = KiwiValid_CheckBrushIgnoringFace(
                                  m_node->def, twin >= 0 ? m_face : -1, &why )
                           && KiwiValid_BrushCloses( m_node->def, &why );
            KiwiValid_Restore( m_base );            // restore the original before deciding

            if ( !ok )
            {
                m_rejected = true;
                UpdateHud();
                Sys_Printf( "Match Face: rejected — %s.  Nothing was changed.\n",
                            why ? why : "invalid geometry" );
                g_nUpdateBits = -1;
                return true;                        // stay running: pick another face
            }

            // Real apply: undo bracket, then the ported texture-lock sequence.
            KiwiCmd_UndoBegin( "match face" );      // clones the untouched original
            KiwiCmd_UndoCoverBrush( m_node );               // face selections are absent from selected_brushes

            // The trial restored the original; removing the source directly yields
            // the gated result because its proposed plane duplicates `twin`.
            // Whole face_t records move, so surviving planes/materials need no reproject.
            if ( twin >= 0 )
            {
                Brush_RemoveFace( m_node->def, (unsigned int)m_face );
                KiwiValid_Rebuild( m_node->def );

                // Later face indices now name different surfaces; discard every cached index.
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

            // Ported texture-lock ABI requires 19 floats and lockFlags[2] == 1.
            byte lockFlags[3];
            lockFlags[0] = (byte)( g_PrefsDlg->m_bTextureLock  != 0 );
            lockFlags[1] = (byte)( g_PrefsDlg->m_bLightmapLock != 0 );
            lockFlags[2] = 1;
            float saveBuf[19];

            face_t *f = &m_node->def->faces[m_face];
            memcpy( f->mtldef, m_baseMtl, sizeof( m_baseMtl ) );                   // restore typed baseline
            Face_MakePlane( f );                    // baseline plane must exist before the save
            if ( f->w && slid )
                Ed_FaceTexLockSave( saveBuf, f );

            WritePlanePts( pts );

            // Unslid points carry no texcoord meaning: leave the texdef alone, which for
            // a planar projection is already the "nothing moved in the plane" answer.
            if ( f->w && slid )
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
            // Trial always restores itself; real apply immediately commits.
            // Bracket helpers self-guard when nothing was opened.
            m_haveTarget = false;
            g_nUpdateBits |= 1;
        }

        // Draw the source boundary, its projection along the source normal, and a centre link.
        // The ghost is approximate because the real rebuild reclips the face.
        void DrawWorld() override
        {
            // The patch arm previews its control net instead of a source winding.
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
                return;                                       // edge-on: projection is undefined

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
        // Latch one object-selected flat control net as the patch source.
        // Refuse at Begin so an invalid source does not consume a target pick.
        bool BeginPatchSource()
        {
            const selection_t &sel  = KiwiSel();
            selbrush_t        *node = 0;
            int                n    = 0;
            for ( size_t i = 0; i < sel.items.size(); ++i )
            {
                const sel_item_t &it = sel.items[i];
                // Patches are object selections; their control points belong to vertex mode.
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

        // Patches are not brush-clipped, so solve and gate all points in scratch.
        // Open undo only after every refusal check passes.
        bool ClickPatch()
        {
            patchMesh_t *pm = PatchMeshOf( m_node );
            if ( !pm || pm != m_srcPatch )
            {
                Sys_Printf( "Match Face: the curve went away.\n" );
                return false;                       // COMMIT — nothing was changed
            }

            // Re-derive because the patch may have been reshaped since Begin.
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

            // BuildPlanePts supplies a unit normal and m_pts[1] on the target plane.
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

            // Scratch solve and coordinate-bound check.
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
            // The cosine gate bounds amplification, not target distance; also cap
            // travel at the V7 map-span limit.
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

            // UndoCoverBrush is deduplicated (undo.cpp:502) but still covers mode-change gaps.
            KiwiCmd_UndoBegin( "match face" );
            KiwiCmd_UndoCoverBrush( m_node );

            for ( int col = 0; col < pm->width; ++col )
                for ( int row = 0; row < pm->height; ++row )
                    Copy3( moved[col][row], pm->ctrl[col][row].xyz );

            // 0x43D800: one rebuild updates bounds, brush, tessellation, and version.
            Patch_Rebuild( pm, 1 );
            MarkMapModified();

            Sys_Printf( "Match Face: curve pushed onto the face (%ix%i control points, "
                        "%.4g unit(s) at most).\n", pm->width, pm->height, (double)maxT );
            g_nUpdateBits = -1;
            return false;                           // COMMIT
        }

        // Preview the current net, projected net, and source-to-target link.
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
                return;                     // do not preview a result ClickPatch refuses

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

        // Build well-spread target-plane points with the source's outward sense.
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

            // Seed from the least-aligned world axis so the in-plane basis cannot collapse.
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
            // Point order below relies on the right-handed relation u x v == n.

            Mad3( c, u, r, pts[0] );
            Copy3( c, pts[1] );
            Mad3( c, v, r, pts[2] );
            Copy3( n, outN );
            return true;
        }

        // The source face's current plane points, each slid along the source normal onto
        // the target plane.  A shear along n with n.N > 0 (BuildPlanePts flips N to make
        // it so) keeps the three points non-collinear and their winding sense.  False
        // when the face is edge-on to the target: the slide is then ill-conditioned.
        bool SlidePlanePts( float out[3][3] ) const
        {
            const float denom = Dot3( m_srcNormal, m_ghostN );
            if ( fabsf( denom ) < KMATCH_MIN_COS )
                return false;
            const float   planeD = Dot3( m_ghostN, m_pts[1] );
            const face_t *f      = &m_node->def->faces[m_face];
            for ( int p = 0; p < 3; ++p )
            {
                ProjectAlong( f->planepts[p], m_srcNormal, m_ghostN, planeD, denom, out[p] );
                for ( int k = 0; k < 3; ++k )
                    if ( !( fabsf( out[p][k] ) <= KVALID_MAX_COORD ) )
                        return false;
            }
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
            // Name the active arm because face and patch modes move different geometry.
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
        // Non-null selects the patch arm and latches mesh identity for liveness checks.
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
        // Mirror face_t::mtldef so both memcpy sizes stay type-derived.
        MaterialDef     m_baseMtl[4];
        char            m_hud[128] = { 0 };
    };

    KiwiMatchCommand s_match;
}

// Offer for one face or, with no faces, one patch object.
// Begin performs the costlier winding and planarity validation.
bool KiwiMatch_CanMatch()
{
    const selection_t &sel = KiwiSel();
    int faces = 0, patches = 0;
    for ( size_t i = 0; i < sel.items.size(); ++i )
    {
        const sel_item_t &it = sel.items[i];
        // Stored selbrush_t pointers must pass Sel_BrushLive before dereference.
        if ( !Sel_BrushLive( it.brush ) )
            continue;
        if ( it.kind == SEL_FACE )
            ++faces;
        else if ( it.kind == SEL_OBJECT && it.brush->patch )
            ++patches;
    }
    return ( faces == 1 ) || ( faces == 0 && patches == 1 );
}

// Registration and lookup.
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

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
#include "kiwi_ported.h"
#include "kiwi_selection.h"
#include "kiwi_texlock.h"   // Face_TexLockPoints*
#include "kiwi_validity.h"
#include "kiwi_vec.h"     // Dot3/Sub3/etc.

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Ported entry points.
extern int   Face_MakePlane( face_t *face );                           // brush.cpp:4495 (0x470470)
// Drops the source when its matched plane duplicates another half-space.

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
    // `skipFace` (a face already being dropped) is not a twin.
    int CoincidentFace( const brush_t *def, int faceIndex, int skipFace = -1 )
    {
        if ( !def || !def->faces || faceIndex < 0 || faceIndex >= def->faceCount )
            return -1;
        const plane_t &pf = def->faces[faceIndex].plane;
        for ( int i = 0; i < def->faceCount; ++i )
        {
            if ( i == faceIndex || i == skipFace )
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
        // One selected brush face and what a click computes for it.
        struct matchSrc_t
        {
            selbrush_t *node = 0;
            int         face = -1;
            float       n[3] = { 0.0f, 0.0f, 1.0f };    // its plane normal at Begin
            // Mirror face_t::mtldef so both memcpy sizes stay type-derived.
            MaterialDef mtl[4];                         // texdef baseline (snapshot omits materials)
            float       pts[3][3];                      // its plane points slid onto the target
            float       exact[3][3];                    // the target's own points, in its sense
            bool        slid = false;
            bool        twin = false;                   // its matched plane duplicates another face
        };

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
            m_srcs.clear();
            m_bases.clear();
            m_hud[0]     = '\0';

            // KIWI (2026-10-08, user: "edit the match face feature so it will work with
            // multiple faces that are shift-selected already"): every selected brush face is
            // a source; one target click gives all of them its plane.
            const selection_t &sel = KiwiSel();
            for ( size_t i = 0; i < sel.items.size(); ++i )
            {
                const sel_item_t &it = sel.items[i];
                if ( it.kind != SEL_FACE || !Sel_BrushLive( it.brush ) )
                    continue;
                if ( it.brush->patch )
                {
                    Sys_Printf( "Match Face: a curve face is selected — only brush faces "
                                "can be matched.\n" );
                    return false;
                }
                if ( IsSource( it.brush, it.faceIndex ) )
                    continue;
                matchSrc_t s;
                s.node = it.brush;
                s.face = it.faceIndex;
                m_srcs.push_back( s );
            }

            // A face selection takes precedence; only zero faces selects the patch arm.
            if ( m_srcs.empty() )
                return BeginPatchSource();

            for ( size_t i = 0; i < m_srcs.size(); ++i )
            {
                matchSrc_t &s   = m_srcs[i];
                brush_t    *def = s.node->def;
                if ( !def || !def->faces || s.face < 0 || s.face >= def->faceCount
                  || !def->faces[s.face].w )
                {
                    Sys_Printf( "Match Face: a selected face has no winding.\n" );
                    m_srcs.clear();
                    m_bases.clear();
                    return false;
                }
                memcpy( s.mtl, def->faces[s.face].mtldef, sizeof( s.mtl ) );   // snapshot omits materials
                Copy3( def->faces[s.face].plane.normal, s.n );

                // The baseline the trial rolls back to, one per brush —
                // KiwiValid_Snapshot only knows planepts / control points.
                if ( BaseOf( def ) )
                    continue;
                m_bases.push_back( kiwiBaseBrush_t() );
                if ( !KiwiValid_Snapshot( def, &m_bases.back() ) )
                {
                    m_srcs.clear();
                    m_bases.clear();
                    return false;
                }
            }
            m_node = m_srcs[0].node;
            Copy3( m_srcs[0].n, m_srcNormal );      // the target's sense follows the first face

            UpdateHud();
            if ( m_srcs.size() == 1 )
                Sys_Printf( "Match Face: click the face to copy the plane FROM "
                            "(Esc cancels).\n" );
            else
                Sys_Printf( "Match Face: %i faces — click the face to copy the plane FROM "
                            "(Esc cancels).\n", (int)m_srcs.size() );
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
                  && !IsSource( hit.item.brush, hit.item.faceIndex ) )
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
            // Both arms share the target guard above.
            if ( m_srcPatch )
            {
                if ( !Sel_BrushLive( m_node ) )
                {
                    Sys_Printf( "Match Face: the source curve went away.\n" );
                    return false;
                }
                return ClickPatch();
            }
            for ( size_t i = 0; i < m_srcs.size(); ++i )
                if ( !Sel_BrushLive( m_srcs[i].node ) )
                {
                    Sys_Printf( "Match Face: a source brush went away.\n" );
                    return false;
                }

            // KIWI (2026-09-21, user: "the UV's sometimes get stretched/blown out. all the
            // command should do is a precise extrude"): the plane points written are the
            // face's OWN three points slid along its normal onto the target plane - what
            // push/pull writes (kiwi_transform.cpp ApplyFaces), with a per-point distance.
            // The ported lock keeps the saved S/T AT THE THREE PLANE POINTS, so it is only
            // meaningful when the new points are the old ones moved.  m_pts (the target's
            // centre +/- its radius along an arbitrary basis) are unrelated points, and
            // with Texture Lock on the solve mapped the old texcoords onto them: an
            // arbitrary affine stretch.  "Sometimes" = whenever the lock pref was on.
            //
            // KIWI (2026-10-05, precision gauntlet): the STORED plane is the target face's own
            // three points (oriented like the source).  The slid points lie on a plane rebuilt
            // from the target's normal and winding centre - a float near-copy, which cod4map
            // (planes merge only within 1e-7 / 0.001) keeps as a second plane beside the one
            // the face was matched to.  The slid points still drive the texture-lock solve
            // below; a planar projection does not depend on which points define the plane.
            for ( size_t i = 0; i < m_srcs.size(); ++i )
            {
                matchSrc_t &s = m_srcs[i];
                s.slid = SlidePlanePts( s, s.pts );
                if ( !TargetPlanePts( s.n, s.exact ) )
                {
                    m_rejected = true;
                    UpdateHud();
                    Sys_Printf( "Match Face: rejected — the target face went away.  Nothing "
                                "was changed.\n" );
                    return true;
                }
            }

            // Trial: no undo bracket or texture lock.  Every source is written, then each
            // brush is rebuilt and gated once with all of its matched faces in place.
            for ( size_t i = 0; i < m_srcs.size(); ++i )
                WritePlanePts( m_srcs[i], m_srcs[i].exact );

            const char *why = 0;
            size_t      bad = 0;
            for ( size_t b = 0; b < m_bases.size() && !why; ++b )
            {
                brush_t *def = m_bases[b].def;
                KiwiValid_Rebuild( def );

                // A copied chamfer plane may duplicate an existing V5 half-space.  If so,
                // gate with that source omitted and require V8 closure; V5 stays strict.
                // Sources are the only faces this edit may remove, one per brush (the
                // gate can leave out one face).  Two sources matched onto the same plane
                // keep the later one and drop the earlier.
                int redundant = -1;
                for ( size_t i = 0; i < m_srcs.size(); ++i )
                {
                    matchSrc_t &s = m_srcs[i];
                    if ( s.node->def != def )
                        continue;
                    s.twin = false;
                    if ( CoincidentFace( def, s.face, redundant ) < 0 )
                        continue;
                    if ( redundant >= 0 )
                    {
                        why = "two of the faces on one brush would both become redundant — "
                              "match those one at a time";
                        bad = i;
                        break;
                    }
                    redundant = s.face;
                    s.twin    = true;
                }
                if ( why )
                    break;
                const char *w = "invalid geometry";
                if ( !KiwiValid_CheckBrushIgnoringFace( def, redundant, &w )
                  || !KiwiValid_BrushCloses( def, &w ) )
                {
                    why = w ? w : "invalid geometry";
                    for ( bad = 0; bad < m_srcs.size() && m_srcs[bad].node->def != def; ++bad )
                        ;
                }
            }
            for ( size_t b = 0; b < m_bases.size(); ++b )
                KiwiValid_Restore( m_bases[b] );    // restore the originals before deciding

            if ( why )
            {
                m_rejected = true;
                UpdateHud();
                if ( m_srcs.size() == 1 )
                    Sys_Printf( "Match Face: rejected — %s.  Nothing was changed.\n", why );
                else
                    Sys_Printf( "Match Face: rejected — %s (selected face %i of %i).  Nothing "
                                "was changed.\n", why, (int)bad + 1, (int)m_srcs.size() );
                g_nUpdateBits = -1;
                return true;                        // stay running: pick another face
            }

            // Real apply: one undo bracket over every brush, then the ported texture-lock
            // sequence per face.
            KiwiCmd_UndoBegin( "match face" );      // clones the untouched originals
            for ( size_t b = 0; b < m_bases.size(); ++b )
                for ( size_t i = 0; i < m_srcs.size(); ++i )
                    if ( m_srcs[i].node->def == m_bases[b].def )
                    {
                        KiwiCmd_UndoCoverBrush( m_srcs[i].node );   // face selections are absent from selected_brushes
                        break;
                    }

            byte lockFlags[3];
            lockFlags[0] = (byte)( g_PrefsDlg->m_bTextureLock  != 0 );
            lockFlags[1] = (byte)( g_PrefsDlg->m_bLightmapLock != 0 );
            lockFlags[2] = 1;

            int matched = 0, removed = 0;
            for ( size_t b = 0; b < m_bases.size(); ++b )
            {
                brush_t *def  = m_bases[b].def;
                int      drop = -1;
                for ( size_t i = 0; i < m_srcs.size(); ++i )
                {
                    matchSrc_t &s = m_srcs[i];
                    if ( s.node->def != def )
                        continue;
                    // The trial restored the original; removing a twin source directly
                    // yields the gated result because its proposed plane duplicates another.
                    if ( s.twin )
                    {
                        drop = s.face;
                        continue;
                    }

                    double saveBuf[3][KTEXLOCK_LAYERS][6];
                    face_t *f = &def->faces[s.face];
                    memcpy( f->mtldef, s.mtl, sizeof( s.mtl ) );           // restore typed baseline
                    Face_MakePlane( f );            // baseline plane must exist before the save
                    if ( f->w && s.slid )
                        Face_TexLockPointsSave( f, saveBuf );

                    WritePlanePts( s, s.slid ? s.pts : s.exact );

                    // Unslid points carry no texcoord meaning: leave the texdef alone, which
                    // for a planar projection is already the "nothing moved in the plane" answer.
                    if ( f->w && s.slid )
                        Face_TexLockPointsReproject( f, saveBuf, lockFlags );
                    WritePlanePts( s, s.exact );    // the same plane, on the target's own points
                    ++matched;
                }

                // Whole face_t records move, so surviving planes/materials need no reproject.
                // Removed last: later face indices on this brush shift down.
                if ( drop >= 0 )
                {
                    Brush_RemoveFace( def, (unsigned int)drop );
                    ++removed;
                }
                KiwiValid_Rebuild( def );
            }

            if ( removed > 0 )
            {
                // Later face indices now name different surfaces; discard every cached index.
                m_srcs.clear();
                Sel_Clear( KiwiSel() );
                Sel_RebuildFromLegacy();
            }

            if ( removed == 0 )
                Sys_Printf( matched == 1 ? "Match Face: plane copied.\n"
                                         : "Match Face: plane copied to %i faces.\n", matched );
            else if ( matched == 0 && removed == 1 )
                Sys_Printf( "Match Face: the face is now coplanar with its neighbour — the "
                            "redundant plane was removed and the edge restored.\n" );
            else
                Sys_Printf( "Match Face: plane copied to %i face(s); %i face(s) became "
                            "coplanar with a neighbour and their redundant planes were "
                            "removed.\n", matched, removed );
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

            for ( size_t s = 0; s < m_srcs.size(); ++s )
                if ( !DrawFacePreview( m_srcs[s] ) )
                    return;
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
                Sys_Printf( "Match Face: select one or more brush faces — or ONE flat "
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

            m_node     = node;              // no face sources in this arm, ever
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

        // One source face: its boundary, its projection along its own normal onto the
        // target plane, and a centre link.  False once the line buffer is full.
        bool DrawFacePreview( const matchSrc_t &s ) const
        {
            if ( !Sel_BrushLive( s.node ) )
                return true;
            winding_t *sw = WindingOf( s.node, s.face );
            if ( !sw || sw->numpoints < 3 || sw->numpoints > MAX_POINTS_ON_WINDING )
                return true;

            KiwiLines_Color( KMATCH_SRC_COL[0], KMATCH_SRC_COL[1], KMATCH_SRC_COL[2] );
            for ( int i = 0; i < sw->numpoints; ++i )
                if ( !KiwiLines_Add( sw->p[i], sw->p[( i + 1 ) % sw->numpoints] ) )
                    return false;

            if ( !m_haveTarget )
                return true;

            // The slide t = (d - N.p) / (N.n) is the same for N and -N, so the target's
            // sense (set by the first source) does not matter here.
            const float d = Dot3( m_ghostN, m_pts[1] );      // pts[1] is the plane point
            const float denom = Dot3( m_ghostN, s.n );
            if ( fabsf( denom ) < KMATCH_EPS )
                return true;                                  // edge-on: projection is undefined

            KiwiLines_Color( KMATCH_GHOST_COL[0], KMATCH_GHOST_COL[1], KMATCH_GHOST_COL[2] );
            float prev[3];
            float first[3];
            for ( int i = 0; i < sw->numpoints; ++i )
            {
                float p[3];
                const float t = ( d - Dot3( m_ghostN, sw->p[i] ) ) / denom;
                Mad3( sw->p[i], s.n, t, p );
                if ( i == 0 )
                    Copy3( p, first );
                else if ( !KiwiLines_Add( prev, p ) )
                    return false;
                Copy3( p, prev );
            }
            if ( !KiwiLines_Add( prev, first ) )
                return false;

            float sc[3], tc[3], r;
            winding_t *tw = WindingOf( m_target.brush, m_target.faceIndex );
            if ( WindingCentre( sw, sc, &r ) && WindingCentre( tw, tc, &r ) )
            {
                KiwiLines_Color( KMATCH_LINK_COL[0], KMATCH_LINK_COL[1], KMATCH_LINK_COL[2] );
                return KiwiLines_Add( sc, tc );
            }
            return true;
        }

        // The source face's current plane points, each slid along the source normal onto
        // the target plane.  A shear along n keeps the three points non-collinear and
        // their winding sense (the slide is the same for N and -N).  False when the face
        // is edge-on to the target: the slide is then ill-conditioned.
        bool SlidePlanePts( const matchSrc_t &s, float out[3][3] ) const
        {
            const float denom = Dot3( s.n, m_ghostN );
            if ( fabsf( denom ) < KMATCH_MIN_COS )
                return false;
            const float   planeD = Dot3( m_ghostN, m_pts[1] );
            const face_t *f      = &s.node->def->faces[s.face];
            for ( int p = 0; p < 3; ++p )
            {
                ProjectAlong( f->planepts[p], s.n, m_ghostN, planeD, denom, out[p] );
                for ( int k = 0; k < 3; ++k )
                    if ( !( fabsf( out[p][k] ) <= KVALID_MAX_COORD ) )
                        return false;
            }
            return true;
        }

        // The target face's own plane points, reversed when its normal opposes the source's
        // `srcN` (each source keeps its own outward sense).  False: no brush-face target.
        bool TargetPlanePts( const float *srcN, float out[3][3] ) const
        {
            const selbrush_t *tb = m_target.brush;
            if ( !Sel_BrushLive( m_target.brush ) || !tb->def || !tb->def->faces
              || m_target.faceIndex < 0 || m_target.faceIndex >= tb->def->faceCount )
                return false;
            const face_t *tf   = &tb->def->faces[m_target.faceIndex];
            const bool    flip = Dot3( tf->plane.normal, srcN ) < 0.0f;
            for ( int p = 0; p < 3; ++p )
                Copy3( tf->planepts[flip ? 2 - p : p], out[p] );
            return true;
        }

        void WritePlanePts( const matchSrc_t &s, const float pts[3][3] )
        {
            face_t *f = &s.node->def->faces[s.face];
            for ( int p = 0; p < 3; ++p )
                Copy3( pts[p], f->planepts[p] );
        }

        bool IsSource( const selbrush_t *node, int face ) const
        {
            for ( size_t i = 0; i < m_srcs.size(); ++i )
                if ( m_srcs[i].node == node && m_srcs[i].face == face )
                    return true;
            return false;
        }

        const kiwiBaseBrush_t *BaseOf( const brush_t *def ) const
        {
            for ( size_t i = 0; i < m_bases.size(); ++i )
                if ( m_bases[i].def == def )
                    return &m_bases[i];
            return 0;
        }

        void UpdateHud()
        {
            // Name the active arm because face and patch modes move different geometry.
            char faces[32];
            _snprintf( faces, sizeof( faces ), "match %i faces", (int)m_srcs.size() );
            faces[sizeof( faces ) - 1] = '\0';
            const char *what = m_srcPatch ? "match curve" : ( m_srcs.size() > 1 ? faces : "match face" );
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

        // Face arm: every source, and one trial baseline per distinct brush.
        std::vector<matchSrc_t>      m_srcs;
        std::vector<kiwiBaseBrush_t> m_bases;
        // Patch arm source; the face arm keeps its first source here.
        selbrush_t     *m_node = 0;
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
        char            m_hud[128] = { 0 };
    };

    KiwiMatchCommand s_match;
}

// Offer for one or more faces or, with no faces, one patch object.
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
    return ( faces >= 1 ) || ( faces == 0 && patches == 1 );
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

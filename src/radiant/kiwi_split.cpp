#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Cut and Face Split UI over the ported brush splitter; see kiwi_split.h for
// plane orientation, ownership, validity, and undo contracts.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"                // camera_s
#include "prefs.h"                  // g_PrefsDlg->m_bNoClamp (exact split planes)
#include <gfx_d3d/r_gfx.h>          // GfxColor
#include <gfx_d3d/r_material.h>     // Material
#include <gfx_d3d/r_rendercmds.h>   // MaterialTechniqueType, TECHNIQUE_UNLIT

#include "kiwi_split.h"
#include "kiwi_camera.h"                    // cut-disc world/pixel scale
#include "kiwi_command.h"
#include "kiwi_fmt.h"
#include "kiwi_focus.h"                     // Ctrl+R brush mode: the clicked face
#include "kiwi_construct.h"
#include "kiwi_conselect.h"                 // Cut fence seeded from selected lines
#include "kiwi_grid.h"                      // world-anchored lattice
#include "kiwi_lines.h"
#include "kiwi_material.h"                  // split-face inheritance
#include "kiwi_numeric.h"                   // Face Split offset
#include "kiwi_pick.h"
#include "kiwi_ported.h"
#include "kiwi_region.h"                    // Ctrl+R on construction faces
#include "kiwi_selection.h"
#include "kiwi_snap.h"                      // geometry snap classification
#include "kiwi_units.h"                     // HUD offset display
#include "kiwi_validity.h"
#include "kiwi_vec.h"                       // shared vector helpers

#include <algorithm>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>

// Ported entry points.
extern void        Brush_SplitBrushByFace( brush_t *in, face_t *face,
                                           brush_t **front, brush_t **back );  // brush.cpp:4605 (0x471960)

namespace
{
    // 0.22 matches region fills; Plasticity's 0.10 is unreadable at KIWI brightness.
    const float KSPLIT_PLANE_RGBA[4] = { 1.00f, 0.16f, 0.16f, 0.22f };
    const float KSPLIT_LINE_COL[3]   = { 1.00f, 0.35f, 0.30f };   // the intersection outline
    const float KSPLIT_EDGE_COL[3]   = { 1.00f, 0.75f, 0.35f };   // the cut LINE itself
    // Match the editor's existing hot-handle yellow.
    const float KSPLIT_HOT_COL[3]    = { 1.00f, 0.90f, 0.30f };

    const float KSPLIT_EPS = 1.0e-4f;

    // World units: keep the cut away from both span ends so neither half is an
    // immediate §19 sliver.  (Snaps project onto the slide axis from any plane.)
    const float KSPLIT_MIN_SPAN  = 1.0f;
    // KIWI (2026-10-26, user: "im trying to split this smaller region and it wont let me"):
    // splits that only lay CONSTRUCTION (region split, face-mode split) answer to the region
    // weld, not to brush slivers.  An edge longer than KREG_JOIN_DIST survives chaining, so
    // a cut line that long and pieces that wide survive; any region that exists at all is
    // wider than the weld, so its cross line always fits.  The old 1-unit floor refused
    // every region under an inch across.
    const float KSPLIT_CON_MIN_EDGE = KREG_JOIN_DIST * 1.05f;
    // Region split: both pieces must outlive the weld, so the span holds two of them.
    const float KSPLITREG_MIN_SPAN  = 2.0f * KSPLIT_CON_MIN_EDGE;
    const char *const KSPLIT_TOO_SMALL_MSG =
        "Split Face: that face is too narrow to split either way.\n";

    // The centroid is both drawn and latched within screen-constant 8 px; Ctrl
    // suppresses the latch with other snapping.
    const float KSPLIT_CENTRE_SNAP_PIX = 8.0f;
    // Split-specific ink distinguishes the persistent target from a live snap hit.
    const float KSPLIT_CENTRE_COL[3] = { 0.55f, 0.95f, 0.80f };

    // Bounds-relative preview stays visible without becoming a map-wide sheet.
    const float KSPLIT_SPAN_SCALE  = 1.6f;    // half-length along the line
    const float KSPLIT_DEPTH_SCALE = 2.4f;    // sweep depth away from the camera


    // Plane as (normal, dist); side tests need consistency, not a preferred sign.
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

    // Match the ported clipper/CSG exclusions: no patches or fixed-size entities.
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

    // Construction segments are picked independently of selection mode at the shared
    // tolerance. Lines and edges sweep from the view; faces supply their own plane.
    // PICKF_EXCLUDE_SELECTED prevents a target brush from lending its own cutter.
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

    // Worst-case pieces grow as 2^N; 64 still admits a maximum-resolution circle.
    const int KCUT_MAX_FENCE = 64;

    // Adapt the shared segment picker to the cut source's ranking payload.
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

    // Shared pick ordering makes an edge win over its face; vertices name no plane.
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

        // Recompute distance from the current winding rather than stale planepts.
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
        out->d    = Dot3( n, f->planepts[1] );     // KIWI: a plane point, not a (CM-snapped) winding point
        out->dist = r.screenDist;
        return true;
    }

    // Line-like hits compete by pixel distance; the construction line is intended
    // to win an exact tie. A face is the fallback area hit.
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

    // Only selects the prompt wording; brush edges/faces make lines optional.
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

    // Filled previews use the ported MATERIAL_COLOR bracket at camwnd.cpp 0x408106;
    // lines cannot preserve alpha. Scale the disc from the published snap-ring size.
    const float KSPLIT_DISC_SCALE = 2.5f;   // of KiwiSnap_RingPixels()
    const int   KSPLIT_DISC_SEGS = 24;      // smooth at the intended ~15 px radius
    const float KSPLIT_DISC_RGBA[4] = { 1.00f, 0.90f, 0.30f, 0.22f };

    // A null normal makes line/edge discs face the camera rather than disappear edge-on.
    void DrawDisc( const float centre[3], const float *normal, float radius,
                   const float rgba[4] )
    {
        static const float s_neutral[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        static const float s_white[4]   = { 1.0f, 1.0f, 1.0f, 1.0f };

        // Ed_Camera is non-null by contract (camwnd.cpp:159).
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

        // white_tools backface-culls, so orient +n toward the eye before winding.
        {
            float toEye[3];
            for ( int k = 0; k < 3; ++k )
                toEye[k] = cam->origin[k] - centre[k];
            if ( Dot3( n, toEye ) < 0.0f )
                for ( int k = 0; k < 3; ++k ) n[k] = -n[k];
        }

        // Seed from the least-aligned world axis, matching KiwiCon_MakePlane.
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

        // World units toward the eye avoid z-fighting with the source face.
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
                const float a = ( (float)( 2.0 * KIWI_PI ) * (float)( i - 1 ) )
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
            // Batch normals are world-constant; vpn is used only for the depth nudge.
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
        // Per-call because KiwiTris_OrientToEye rewrites winding for the current eye.
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
            KiwiTris_FillNormal( nrm[i] );   // constant world normal required by batcher
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

    // KIWI (2026-09-28, user: "annoying behaviors with the snapping of the ctrl-R tool. the
    // alignment isn't working in the same space"): the cut position comes from the point of
    // the slide AXIS (base + s * dir) nearest the cursor ray - a face seen edge-on no longer
    // throws a plane hit far off - and any geometry snap anywhere in the map projects onto
    // that axis, so the cut lines up with a corner on another plane.
    bool RayAxisClosest( const ray_t &ray, const float base[3], const float dir[3], float out[3] )
    {
        float w0[3];
        Sub3( base, ray.origin, w0 );
        const float b = Dot3( dir, ray.dir );
        const float c = Dot3( ray.dir, ray.dir );
        const float d = Dot3( dir, w0 );
        const float e = Dot3( ray.dir, w0 );
        const float den = c - b * b;          // |dir| == 1
        if ( den < 1.0e-6f * c )
            return false;                     // the ray runs along the axis
        const float s = ( b * e - c * d ) / den;
        Mad3( base, dir, s, out );
        return true;
    }

    // A convex face meets a plane in at most one segment; their union is the outline.
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

// Shared splitter; contracts are in kiwi_split.h.
bool KiwiSplit_PlaneCrossesBrush( const brush_t *def,
                                  const float p0[3], const float p1[3], const float p2[3] )
{
    if ( !def )
        return false;
    float n[3], d;
    if ( !PlaneOf( p0, p1, p2, n, &d ) )
        return false;

    // Bounds straddling is conservative: a false positive becomes a handled NULL half.
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

// The def-level body supports repeated off-map splitting. Brush_SplitBrushByFace
// at brush.cpp:4605 gives the template face to back and its reverse to front:
// back = {n·p <= d}, front = {n·p >= d}. Subtract depends on this orientation.
// The ordinary wrapper remains all-or-nothing and reports front's §19 failure first.
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
                               false, 0, why ) )   // ordinary all-or-nothing split
        return false;

    if ( fs == KSPLIT_HALF_SLIVER || bs == KSPLIT_HALF_SLIVER )
    {
        // Never expose the surviving half of an ordinary refused split.
        if ( front ) KiwiSplit_FreeUnlandedDef( front );
        if ( back  ) KiwiSplit_FreeUnlandedDef( back  );
        return false;
    }

    if ( outFront ) *outFront = front;
    if ( outBack  ) *outBack  = back;
    return true;
}

// `keepRefusedBack` is the only carve divergence; front is always a landed candidate.
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

    // Reject a degenerate plane before the ported core clones anything.
    float cutN[3], cutD;
    if ( !PlaneOf( p0, p1, p2, cutN, &cutD ) )
    {
        *why = "degenerate cut plane";
        return false;
    }

    // Split walls inherit from the best source face; all-tool brushes fall back to
    // classic caulk. A zeroed template intentionally leaves `w` NULL for Face_Alloc.
    face_t clipFace{};
    KiwiMtl_SeedClipFace( &clipFace, def, cutN );
    // Repair missing inherited layers; the caulk fallback already supplies them.
    KiwiMtl_EnsureFaceLayers( &clipFace );        // kiwi_material.h:245
    for ( int k = 0; k < 3; ++k )
    {
        clipFace.planepts[0][k] = p0[k];
        clipFace.planepts[1][k] = p1[k];
        clipFace.planepts[2][k] = p2[k];
    }

    // KIWI (2026-09-28, user: a face "created with the ctrl-R split command ... should be
    // fine" would not extrude): the ported core rebuilds both halves with bFull = 1, whose
    // Brush_SnapPlanepts rounds every planept to 0.5.  That moved the cut off the previewed
    // line by up to 0.25 per axis and, on a wall that is not axis-aligned, tilted it - a cut
    // near an existing edge left a ~0.1-unit sliver (float32 replica: 148 x 0.10 on the
    // 1.5-degree powerplant wall) that no later extrusion could build.  The cut keeps its
    // exact plane: the clamp is suspended for this call only (the pref is unchanged).
    brush_t *front = 0, *back = 0;
    {
        const int savedNoClamp = g_PrefsDlg->m_bNoClamp;
        g_PrefsDlg->m_bNoClamp = 1;
        Brush_SplitBrushByFace( def, &clipFace, &front, &back );
        g_PrefsDlg->m_bNoClamp = savedNoClamp;
    }

    if ( !front && !back )
    {
        *why = "the plane left nothing on either side";
        return false;
    }

    // The ported core rebuilt planes, windings, and bounds. Gate off-list halves
    // independently and free refusals with the CSG_MakeHollow unlink/free pair.
    // Preserve front-before-back reason ordering.
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
        // Back is never landed during subtract, so presentation-only refusals may
        // continue when it has real thickness and remains within the source bounds.
        // One world unit of containment slack covers split rounding.
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
    // A carried back still reports the gate it failed.
    else if ( outBackRefused && *outBackRefused )
        *why = backWhy;

    if ( outFront )      *outFront      = front;
    if ( outBack  )      *outBack       = back;
    if ( outFrontState ) *outFrontState = fs;
    if ( outBackState  ) *outBackState  = bs;
    return true;
}

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

    // This entry point requires two halves; discard a one-sided clone.
    if ( !front || !back )
    {
        KiwiSplit_FreeUnlandedDef( front );
        KiwiSplit_FreeUnlandedDef( back );
        *why = "the plane does not cross this brush";
        return false;
    }

    // Land selected in the ported order; the caller frees the source afterward.
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
    // Split targets and free each source inside the caller's undo bracket.
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
            // Land before free so the owner never transiently loses every brush.
            Brush_Free( node );
            ++done;
        }
        return done;
    }

    // The brush half of C: a selected brush the ported splitter accepts.
    bool CanCutBrushes()
    {
        for ( selbrush_t *n = selected_brushes.next; n != &selected_brushes; n = n->next )
            if ( Splittable( n ) )
                return true;
        return false;
    }

    // KIWI (2026-10-05, user: "allow cutting of lines with another line"): the construction
    // objects C cuts when no brush is selected - every visible object a whole-object or
    // segment item names (a point names no shape), ascending, once each.
    void LineCutTargets( std::vector<int> *out )
    {
        out->clear();
        for ( int i = 0; i < KiwiConSel_Count(); ++i )
        {
            const kconSelItem_t *it = KiwiConSel_At( i );
            if ( !it || it->kind == KCONSEL_POINT || KiwiCon_Hidden( it->object ) )
                continue;                       // Hidden reads true for a bad index
            if ( std::find( out->begin(), out->end(), it->object ) == out->end() )
                out->push_back( it->object );
        }
        std::sort( out->begin(), out->end() );
    }

    bool CanCutLines()
    {
        std::vector<int> t;
        LineCutTargets( &t );
        return !t.empty();
    }

    // C — Cut. The source click locks the plane described in kiwi_split.h.
    class KiwiCutCommand : public KiwiEditorCommand
    {
    public:
        const char *Name() const override { return "Cut"; }
        bool CanExecute() override { return CanCutBrushes(); }

        // Both stages accept clicks so Shift+LMB can grow a fence; RMB/Enter commits.
        bool WantsClicks() const override { return true; }

        // Cut has no numeric field; the picked source defines its plane.
        const char *HudStatus() const override { return m_hud[0] ? m_hud : 0; }
        // Red only when a locked preview crosses no target.
        bool        HudInvalid() const override
        { return m_stage == KCUT_PREVIEW && m_crossing <= 0; }

        // Fence overlay cost grows with planes and targets; zero keeps the default.
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
            // Static strings are required by the HUD contract; both stages advertise
            // the additive fence grammar.
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

            // Lines are optional because a brush edge or face also names a plane.
            m_stage = KCUT_PICK_LINE;

            // KIWI: construction lines Shift-selected BEFORE C seed the fence, so a
            // brush + many lines selected and C goes straight to the locked preview.
            for ( int i = 0; i < KiwiConSel_Count(); ++i )
            {
                const kconSelItem_t *it = KiwiConSel_At( i );
                if ( !it || it->object < 0 || it->kind == KCONSEL_POINT )
                    continue;
                fenceSrc_t s;
                s.object  = it->object;
                s.segment = ( it->kind == KCONSEL_SEGMENT ) ? it->index : -1;
                m_fenceSrc.push_back( s );
            }
            if ( !m_fenceSrc.empty() )
            {
                m_srcKind = KCUT_SRC_LINE;
                if ( LockFence() )
                    return true;
                m_fenceSrc.clear();
                m_fence.clear();
            }

            UpdateHud();
            Sys_Printf( "Cut: click %s, a brush EDGE, or a brush FACE to use its "
                        "plane (Esc cancels).\n",
                        AnyConstructionSegment() ? "a construction line"
                                                 : "a construction line (there are none)" );
            return true;
        }

        // Mouse motion updates only the next source hover. Click alone writes the
        // locked plane, so orbiting during preview cannot re-aim it.
        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick;
            // Keep hover live in preview so the next Shift+click can be aimed.
            const bool preview = ( m_stage != KCUT_PICK_LINE );

            // Stage-1 disc follows the snap point so it agrees with the snap marker.
            m_haveDisc = !preview && snap.valid;
            if ( m_haveDisc )
                Copy3( snap.position, m_discPt );

            int   x, y;
            const bool      was     = m_haveHover;
            const cutPick_t wasWhat = m_hover;
            m_haveHover = false;
            if ( KiwiCmd_LastCursor( &x, &y ) )
                m_haveHover = PickCutSourceAt( x, y, &m_hover );   // line, edge, or face
            // The disc follows the snap point even within one hovered source.
            g_nUpdateBits |= 1;
            // Also track hover identity changes, including adjacent face areas.
            if ( m_haveHover != was
              || ( m_haveHover && ( m_hover.kind      != wasWhat.kind
                                 || m_hover.object    != wasWhat.object
                                 || m_hover.segment   != wasWhat.segment
                                 || m_hover.node      != wasWhat.node
                                 || m_hover.faceIndex != wasWhat.faceIndex ) ) )
                g_nUpdateBits |= 1;
            UpdateHud();
        }

        // Source clicks lock planes; only RMB/Enter commits.
        bool Click() override
        {
            // KIWI: Shift+LMB on a construction line keeps growing the fence after the
            // first plane locked (the preview prompt advertises it); bare clicks in
            // preview stay inert so a stray press cannot re-aim the locked plane.
            if ( m_stage != KCUT_PICK_LINE )
            {
                if ( !KiwiCmd_LastShift() || !m_haveHover
                  || m_hover.kind != KCUT_SRC_LINE || m_hover.object < 0 )
                    return true;
                if ( m_srcKind != KCUT_SRC_LINE )
                {
                    // An edge/face plane holds no fence; the line starts a new one.
                    m_fenceSrc.clear();
                    m_fence.clear();
                }
                m_stage = KCUT_PICK_LINE;
            }
            if ( !m_haveHover )
            {
                Sys_Printf( "Cut: nothing to cut along there — click a construction "
                            "line, a brush edge, or a brush face, or press Esc.\n" );
                return true;
            }

            m_srcKind = m_hover.kind;
            // Edge/face sources are single planes and cannot retain a construction fence.
            if ( m_hover.kind != KCUT_SRC_LINE )
            {
                m_fenceSrc.clear();
                m_fence.clear();
            }

            if ( m_srcKind == KCUT_SRC_FACE )
            {
                // A face supplies its plane; bounds only size nearby, stable planepts.
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

            // A construction click takes the entire object. Shift toggles additional
            // objects; brush edges remain single view-swept planes.
            if ( m_srcKind == KCUT_SRC_LINE && m_hover.object >= 0 )
            {
                const bool add = KiwiCmd_LastShift();
                if ( !add )
                    m_fenceSrc.clear();
                // A click toggles the WHOLE object: any entry naming it (a whole
                // object or pre-selected segments of it) is dropped, else it joins.
                bool removed = false;
                for ( size_t i = 0; i < m_fenceSrc.size(); )
                {
                    if ( m_fenceSrc[i].object == m_hover.object )
                    {
                        m_fenceSrc.erase( m_fenceSrc.begin() + i );
                        removed = true;
                        continue;
                    }
                    ++i;
                }
                if ( !removed )
                {
                    fenceSrc_t s;
                    s.object = m_hover.object;
                    m_fenceSrc.push_back( s );
                }

                if ( m_fenceSrc.empty() )
                {
                    m_fence.clear();
                    m_stage = KCUT_PICK_LINE;
                    UpdateHud();
                    Sys_Printf( "Cut: nothing selected to cut with — click a line, "
                                "a circle, a brush edge or a brush face.\n" );
                    g_nUpdateBits = -1;
                    return true;
                }

                if ( !LockFence() )
                {
                    m_fenceSrc.clear();
                    m_fence.clear();
                    m_stage = KCUT_PICK_LINE;
                    UpdateHud();
                    Sys_Printf( "Cut: that geometry gives no sweep direction — it "
                                "points at the camera and carries no plane.  Orbit, "
                                "then click it again.\n" );
                }
                return true;
            }

            // Lines and edges share the endpoint-plus-camera derivation.
            Copy3( m_hover.a, m_lineA );
            Copy3( m_hover.b, m_lineB );
            Sub3( m_lineB, m_lineA, m_dir );
            if ( !Norm3( m_dir ) )
            {
                Sys_Printf( "Cut: that segment has zero length — pick another.\n" );
                return true;
            }

            // Bounds provide AimFromCamera's stable plane-point lever arm.
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

        // Esc steps preview back to picking; Enter is consumed until a plane is locked.
        bool KeyDown( int vk, unsigned mods ) override
        {
            (void)mods;
            if ( vk == 0x1B && m_stage == KCUT_PREVIEW )     // VK_ESCAPE
            {
                m_stage     = KCUT_PICK_LINE;
                m_crossing  = 0;
                m_haveHover = false;
                // Defensive only: WantsClicks() currently prevents either stage parking.
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
                // Draw first beneath hover/snap ink; screen-constant and face-oriented
                // when a face supplies a surface.
                if ( m_haveDisc )
                {
                    const float r = KiwiSnap_RingPixels() * KSPLIT_DISC_SCALE
                                  * KiwiCam_WorldPerPixel( m_discPt );
                    const float *onSurface = 0;
                    if ( m_haveHover && m_hover.kind == KCUT_SRC_FACE )
                        onSurface = m_hover.n;
                    DrawDisc( m_discPt, onSurface, r, KSPLIT_DISC_RGBA );
                }

                // Before locking, draw only the source itself; no live sweep plane.
                if ( m_haveHover )
                {
                    KiwiLines_Color( KSPLIT_HOT_COL[0], KSPLIT_HOT_COL[1], KSPLIT_HOT_COL[2] );
                    // Draw the full hovered source: segment/edge or complete face winding.
                    if ( m_hover.kind == KCUT_SRC_FACE )
                        DrawFaceOutline( m_hover );
                    else
                        KiwiLines_Add( m_hover.a, m_hover.b );
                }
                return;
            }

            // Fence planes use outlines: many translucent fills would hide the target.
            if ( FenceActive() )
            {
                const float lift = m_radius;   // bounds-scaled lever arm
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

            // Line/edge quads sweep along the click-time view; face quads are centered
            // patches in the source plane. Both are sized from target bounds.
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

            // Face-derived planes have no source line; m_lineA/B may be stale there.
            if ( !faceMode )
            {
                KiwiLines_Color( KSPLIT_EDGE_COL[0], KSPLIT_EDGE_COL[1], KSPLIT_EDGE_COL[2] );
                KiwiLines_Add( m_lineA, m_lineB );
            }

            KiwiLines_Color( KSPLIT_LINE_COL[0], KSPLIT_LINE_COL[1], KSPLIT_LINE_COL[2] );
            for ( size_t i = 0; i < m_targets.size(); ++i )
                if ( Sel_BrushLive( m_targets[i] ) )
                    DrawIntersection( m_targets[i]->def, m_planeN, m_planeD );

            // KIWI: the next Shift+click's line lights up, as in the fence preview.
            if ( m_haveHover && m_hover.kind == KCUT_SRC_LINE
              && !FenceHasObject( m_hover.object ) )
            {
                KiwiLines_Color( KSPLIT_HOT_COL[0], KSPLIT_HOT_COL[1], KSPLIT_HOT_COL[2] );
                KiwiLines_Add( m_hover.a, m_hover.b );
            }
        }

        void Commit() override
        {
            if ( m_stage != KCUT_PREVIEW )
            {
                Sys_Printf( "Cut: no cutting plane was picked — nothing was cut.\n" );
                Reset();
                return;
            }

            // Re-test live brush geometry at commit; the locked plane cannot change.
            std::vector<selbrush_t *> hit;
            for ( size_t i = 0; i < m_targets.size(); ++i )
            {
                selbrush_t *node = m_targets[i];
                if ( !Sel_BrushLive( node ) )
                    continue;
                if ( FenceActive() )
                {
                    // Any crossing plane hands the brush to the per-plane cascade.
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

            // One undo record clones every source before any split is landed.
            KiwiCmd_UndoBegin( "cut brushes" );
            if ( FenceActive() )
            {
                // The whole fence is one gesture and one undo record.
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
            m_haveDisc  = false;      // stage-1 snap disc
            m_fenceSrc.clear();       // additive construction sources
            m_fence.clear();
            m_fenceFromPlane = false;
            m_hud[0]    = '\0';
        }

        // Re-fetch the hovered face winding because the brush may have rebuilt.
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

        // Preserve the clicked face plane without a camera term. Bounds-scaled nearby
        // planepts improve cross-product precision; the same basis drives the preview.
        bool AimFromFace()
        {
            float n[3];
            Copy3( m_hover.n, n );
            if ( !Norm3( n ) )
                return false;

            // The least-aligned world axis gives a stable in-plane basis.
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

            // Project the unmodified target-bounds center, not the line-arm center.
            float c[3];
            const float d = m_hover.d;
            Mad3( m_mid, n, d - Dot3( n, m_mid ), c );

            const float arm = m_radius;        // bounds-scaled lever arm
            Copy3( c, m_p[0] );
            Mad3( c, e1, arm, m_p[1] );
            Mad3( c, e2, arm, m_p[2] );
            // KIWI (2026-10-05, precision gauntlet): cut on the picked face's OWN plane points.
            // Rebuilt from (n, d) the cut came out ~2e-7 rad / 1e-5 off that face, and cod4map
            // merges planes only within 1e-7 (FindFloatPlane): a wall cut flush to a
            // neighbour's face got two near-identical planes along the joint.
            if ( m_hover.node && Sel_BrushLive( m_hover.node ) && m_hover.node->def
              && m_hover.faceIndex >= 0 && m_hover.faceIndex < m_hover.node->def->faceCount )
            {
                const face_t *sf = &m_hover.node->def->faces[m_hover.faceIndex];
                for ( int k = 0; k < 3; ++k )
                    Copy3( sf->planepts[k], m_p[k] );
            }

            Copy3( n,  m_planeN );
            m_planeD = d;
            Copy3( e1, m_dir );
            Copy3( e2, m_away );
            Copy3( c,  m_centre );
            // Face mode does not draw these; initialize them on-plane defensively.
            Copy3( c, m_lineA );
            Copy3( c, m_lineB );
            return true;
        }

        // Called only by Click: project camera forward off the line and lock the plane.
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

            // Two points stay on the source line; the third uses a bounds-scaled sweep.
            Copy3( m_lineA, m_p[0] );
            Copy3( m_lineB, m_p[1] );
            const float lift = m_radius;       // bounds-scaled lever arm
            Mad3( m_lineA, m_away, lift, m_p[2] );
            return true;
        }

        // Target-union bounds size previews and plane-point lever arms.
        // A construction object contributes every segment; Shift toggles objects.
        // Multi-segment fences sweep along the first available object-plane normal,
        // falling back to the first segment's click-time camera direction.
        // Each plane splits every current piece and keeps both halves, so concave
        // fences intentionally produce multiple convex brushes. One undo covers all.
        bool FenceActive() const { return (int)( m_fence.size() / 6 ) > 1; }
        int  FenceCount()  const { return (int)( m_fence.size() / 6 ); }

        bool FenceHasObject( int obj ) const
        {
            for ( size_t i = 0; i < m_fenceSrc.size(); ++i )
                if ( m_fenceSrc[i].object == obj )
                    return true;
            return false;
        }

        // KIWI: m_fenceSrc -> locked preview.  Several segments cut as a fence; one
        // segment takes the ordinary single-plane path swept from the view.  False
        // leaves the stage untouched (no usable plane).
        bool LockFence()
        {
            MeasureBounds();
            if ( !FenceRebuild() )
                return false;

            if ( FenceActive() )
            {
                // m_p remains single-plane state; fence geometry lives separately.
                m_stage = KCUT_PREVIEW;
                CountCrossings();
                UpdateHud();
                Sys_Printf( "Cut: %i segment(s) from %i source(s), swept along "
                            "%s — every piece is KEPT (a closed loop separates "
                            "inside from outside).  Shift+click adds more, "
                            "RMB / Enter cuts.\n",
                            FenceCount(), (int)m_fenceSrc.size(),
                            m_fenceFromPlane ? "its own plane normal"
                                             : "the view direction" );
                g_nUpdateBits = -1;
                return true;
            }

            // One segment uses the ordinary single-plane path.
            Copy3( &m_fence[0], m_lineA );
            Copy3( &m_fence[3], m_lineB );
            Sub3( m_lineB, m_lineA, m_dir );
            if ( !Norm3( m_dir ) )
                return false;
            MeasureBounds();                    // re-centre on the real line
            if ( !AimFromCamera() )
                return false;
            m_stage = KCUT_PREVIEW;
            CountCrossings();
            UpdateHud();
            Sys_Printf( "Cut: plane locked from this view — Shift+click adds lines, "
                        "RMB / Enter cuts, Esc picks another.\n" );
            g_nUpdateBits = -1;
            return true;
        }

        // Rebuild flattened world segments and their shared sweep direction together.
        bool FenceRebuild()
        {
            m_fence.clear();
            m_fenceFromPlane = false;

            bool haveN = false;
            for ( size_t k = 0; k < m_fenceSrc.size(); ++k )
            {
                const kconObject_t *o = KiwiCon_At( m_fenceSrc[k].object );
                if ( !o || o->hidden )
                    continue;
                const int only = m_fenceSrc[k].segment;     // -1 = every segment

                // Use the first planar member so pick order determines the sweep.
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
                    if ( only >= 0 && s != only )
                        continue;
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
                // Without an object plane, use the first segment's camera derivation.
                Copy3( &m_fence[0], m_lineA );
                Copy3( &m_fence[3], m_lineB );
                Sub3( m_lineB, m_lineA, m_dir );
                if ( !Norm3( m_dir ) || !AimFromCamera() )
                    return false;
                Copy3( m_away, m_fenceN );
            }
            return true;
        }

        // Segment endpoints plus a bounds-scaled sweep form stable planepts.
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
            // A segment parallel to the sweep extrudes to a line; skip it, not the fence.
            float n[3];
            Cross3( e, m_fenceN, n );
            if ( !Norm3( n ) )
                return false;

            const float lift = m_radius;       // bounds-scaled lever arm
            Copy3( a, out[0] );
            Copy3( b, out[1] );
            Mad3( a, m_fenceN, lift, out[2] );
            return true;
        }

        // Apply every plane off-map before landing. Until the first actual split,
        // `cur` contains the borrowed map def; afterward every entry is owned.
        // Return cut targets and report their total landed pieces separately.
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
                            // Earlier entries were freed or moved to `next`; clean from c onward.
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
                            // Cut keeps both halves; boolean subtract drops back.
                            next.push_back( front );
                            next.push_back( back );
                            if ( owned )
                                KiwiSplit_FreeUnlandedDef( cur[c] );
                            changed = true;
                        }
                        else
                        {
                            // On a miss, discard the one-sided clone and retain the original.
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
                // Land every piece before freeing its source; preserve owner lifetime.
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
                // Targets are latched; recheck liveness before reading bounds.
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

            // Keep the unprojected center for both line and face source paths.
            for ( int k = 0; k < 3; ++k )
                m_mid[k] = ( lo[k] + hi[k] ) * 0.5f;

            // Project center onto the source line so short lines still preview full width.
            float rel[3];
            Sub3( m_mid, m_lineA, rel );
            Mad3( m_lineA, m_dir, Dot3( rel, m_dir ), m_centre );
            // Caller runs MeasureBounds before the sole AimFromCamera derivation.
        }

        // Count targets crossed by the locked source; commit re-tests live geometry.
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
                // Name the exact source before it is locked.
                const char *what = "click a line, edge or face";
                if ( m_haveHover )
                    what = ( m_hover.kind == KCUT_SRC_FACE ) ? "click this FACE — its plane cuts"
                         : ( m_hover.kind == KCUT_SRC_EDGE ) ? "click this EDGE — sweeps from the view"
                    // Construction clicks take whole objects, not just hovered segments.
                                                             : "click this SHAPE — all of it cuts";
                _snprintf( m_hud, sizeof( m_hud ),
                           "cut  %i brush(es)  %s", (int)m_targets.size(), what );
            }
            else
            {
                if ( FenceActive() )
                    // Advertise fence plane count because concave fences make many pieces.
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

        // Source picking precedes locked preview.
        enum stage_t { KCUT_PICK_LINE = 0, KCUT_PREVIEW };
        stage_t   m_stage     = KCUT_PICK_LINE;
        bool      m_haveHover = false;
        cutPick_t m_hover;
        // The snap disc can exist independently of a hovered cut source.
        bool      m_haveDisc = false;
        float     m_discPt[3] = { 0.0f, 0.0f, 0.0f };
        cutSrc_t  m_srcKind   = KCUT_SRC_LINE;   // locked plane source

        std::vector<selbrush_t *> m_targets;
        float m_lineA[3]  = { 0.0f, 0.0f, 0.0f };
        float m_lineB[3]  = { 0.0f, 0.0f, 0.0f };
        float m_dir[3]    = { 1.0f, 0.0f, 0.0f };
        float m_away[3]   = { 0.0f, 1.0f, 0.0f };
        float m_centre[3] = { 0.0f, 0.0f, 0.0f };
        float m_mid[3]    = { 0.0f, 0.0f, 0.0f };   // target-bounds center
        float m_planeN[3] = { 0.0f, 0.0f, 1.0f };
        float m_planeD    = 0.0f;
        float m_p[3][3]   = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } };
        // Invariant: MeasureBounds is the only writer and keeps m_radius > 1.
        float m_radius    = 64.0f;
        // Fence sources stay in pick order; flattened segments use six floats each.
        // FenceRebuild updates segments, sweep normal, and its source together.
        // KIWI: segment -1 = the whole object; >= 0 = one pre-selected segment.
        struct fenceSrc_t
        {
            int object  = -1;
            int segment = -1;
        };
        std::vector<fenceSrc_t> m_fenceSrc;
        std::vector<float> m_fence;
        float m_fenceN[3] = { 0.0f, 0.0f, 1.0f };
        bool  m_fenceFromPlane = false;
        int   m_crossing  = 0;
        char  m_hud[128]  = { 0 };
    };

    // Static label lifetime is required; exactly one field preserves Tab for U/V.
    const kiwiNumField_t KSPLITFACE_FIELDS[1] = { { "offset", KNUM_LENGTH, false } };

    // Ctrl+R — live Face Split; Tab flips U/V.
    class KiwiSplitFaceCommand : public KiwiEditorCommand
    {
    public:
        const char *Name() const override { return "Split Face"; }
        bool CanExecute() override { return KiwiSplit_CanSplitFace(); }

        // KIWI: placing a cut is construction - snaps on by default, Ctrl frees (it was
        // the transform context, raw until Ctrl, which the SNAP_NONE logic never expected).
        kiwiSnapCtx_t SnapContext() const override { return KSNAPCTX_CONSTRUCT; }

        // Offset is measured from the low slide-axis edge; one field preserves Tab.
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

        // Put the convex-brush limitation on the pre-commit prompt strip.
        int HudPrompts( const kiwiPrompt_t **out ) const override
        {
            static const kiwiPrompt_t s_prompts[] = {
                { "Tab",  "Flip U / V" },
                { "Move", "Slide the cut" },
                // Advertise the persistent center latch.
                { "Dot",  "Face centre - snaps for an exact half" },
                { "0-9",  "Exact offset" },
                { "!",    "Splits the whole brush" },
            };
            // KIWI (2026-10-22): face mode leaves the brush alone.
            static const kiwiPrompt_t s_facePrompts[] = {
                { "Tab",  "Flip U / V" },
                { "Move", "Slide the cut" },
                { "Dot",  "Face centre - snaps for an exact half" },
                { "0-9",  "Exact offset" },
                { "!",    "Makes two construction faces on the face - extrude either" },
            };
            *out = m_faceOnly ? s_facePrompts : s_prompts;
            return (int)( sizeof( s_prompts ) / sizeof( s_prompts[0] ) );
        }

        bool Begin() override
        {
            m_valid = false;
            m_axis  = 0;                    // U
            m_hud[0] = '\0';
            m_hasNum   = false;
            m_numWorld = 0.0f;
            m_haveT    = false;             // seed at centroid until mouse movement

            m_faceOnly = false;

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
            // KIWI (2026-10-22, user): "Mode (4) Brush: Select a brush (selection requires
            // clicking a face, record this), ctrl-R (same controls as now), entire brush is
            // split" with the line shown around the whole brush.  No face selected: the
            // selected brush splits along the face its selecting click landed on.
            if ( nFaces == 0 )
            {
                selbrush_t *node = 0;
                int         fi   = -1;
                if ( !KiwiFocus_ClickedOrNearestFace( &node, &fi ) || !Sel_BrushLive( node ) )
                {
                    Sys_Printf( "Split Face: select ONE face, or a brush.\n" );
                    return false;
                }
                if ( !Splittable( node ) )
                {
                    Sys_Printf( "Split Face: that brush cannot be split.\n" );
                    return false;
                }
                m_node = node;
                m_face = fi;
                if ( !DeriveEitherAxis() )
                {
                    Sys_Printf( m_tooSmall ? KSPLIT_TOO_SMALL_MSG : "Split Face: that face is degenerate.\n" );
                    return false;
                }
                UpdateHud();
                Sys_Printf( "Split Brush: the cut runs across the face you clicked - move the "
                            "mouse to slide it, Tab flips U/V, type an exact offset, RMB / "
                            "Enter splits the whole brush.\n" );
                return true;
            }
            if ( nFaces != 1 || !face )
            {
                Sys_Printf( "Split Face: select exactly ONE face.\n" );
                return false;
            }
            m_node = face->brush;
            m_face = face->faceIndex;
            // KIWI (2026-10-22, user): "Mode (3) Face: ... just show the line cutting the face,
            // not the entire block ... instead of splitting the entire brush, a new construction
            // face is created from the area and placed against the brush perfectly" - both
            // halves, as two closed construction loops on the face; the brush is not touched.
            m_faceOnly = true;
            if ( !DeriveEitherAxis() )
            {
                Sys_Printf( m_tooSmall ? KSPLIT_TOO_SMALL_MSG : "Split Face: that face is degenerate.\n" );
                return false;
            }
            UpdateHud();
            Sys_Printf( "Split Face: move the mouse to slide the cut, Tab flips U/V, type an "
                        "exact offset, RMB / Enter lays the two halves on the face as "
                        "construction faces (the brush is not split).\n" );
            return true;
        }

        // With one numeric field, kiwi_command.cpp routes Tab here before cycling.
        bool KeyDown( int vk, unsigned mods ) override
        {
            (void)mods;
            if ( vk != 0x09 )               // VK_TAB
                return false;
            m_axis = ( m_axis + 1 ) & 1;
            // The reference edge changes with the axis; drop typed/hovered position
            // and re-seat at the centroid.
            m_haveT = false;
            Derive();
            UpdateHud();
            g_nUpdateBits |= 1;
            return true;
        }

        // Position priority: typed offset, on-plane geometry snap, then grid-snapped
        // cursor-ray intersection with the selected face plane.
        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick;
            // Rebuild the slide basis before mapping this frame's cursor.
            float n[3], d;
            if ( DeriveBasis() && FacePlane( n, &d ) )
            {
                float world[3];
                bool  have = false;

                // KIWI: a geometry snap ANYWHERE projects onto the slide axis (a window
                // corner on the wall above aligns the cut).  SNAP_FACE is the unsnapped
                // surface hit and would starve grid snap.
                const bool tookGeometry = snap.valid && KiwiSnap_IsGeometry( snap.type )
                                       && snap.type != SNAP_FACE;
                if ( tookGeometry )
                {
                    Copy3( snap.position, world );
                    have = true;
                }
                // Otherwise the slide-axis point nearest the cursor ray, grid-snapped.
                // A valid SNAP_NONE signals Ctrl suppression and preserves the raw point.
                const bool suppressed = snap.valid && snap.type == SNAP_NONE;
                if ( !have )
                {
                    ray_t ray;
                    float hit[3];
                    if ( Pick_RayFromCursor( &ray )
                      && ( RayAxisClosest( ray, m_centre, OffDir(), hit )
                        || RayPlane( ray, n, d, hit ) ) )
                    {
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

                    // Apply the screen-space center latch last so explicit geometry
                    // snaps win; SNAP_NONE/Ctrl and typed input suppress it.
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
            if ( !m_faceOnly && Sel_BrushLive( m_node ) )   // KIWI: face mode: the face's line only
                DrawIntersection( m_node->def, m_planeN, m_planeD );

            // Draw the standard point glyph last so the persistent center stays visible.
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

            // KIWI (2026-10-22): face mode - two closed construction loops exactly on the face
            // (both halves), one construction undo step; the brush is untouched.
            if ( m_faceOnly )
            {
                kconPlane_t plane;
                if ( !KiwiCon_MakePlane( m_centre, m_nrm, m_lineDir, &plane ) )
                {
                    Sys_Printf( "Split Face: that face has no usable plane.\n" );
                    return;
                }
                std::vector<float> half[2];
                HalfOfFace( true,  half[0] );
                HalfOfFace( false, half[1] );
                KiwiCon_UndoPush();
                int added = 0;
                for ( int h = 0; h < 2; ++h )
                {
                    if ( half[h].size() < 9 )
                        continue;
                    kconObject_t o;
                    o.type       = KCON_POLYLINE;
                    o.closed     = true;
                    o.plane      = plane;
                    o.planeValid = true;
                    o.pts        = half[h];
                    if ( KiwiCon_Add( o ) >= 0 )
                        ++added;
                }
                Sel_Clear( KiwiSel() );
                Sys_Printf( "Split Face: %i construction face%s laid on the face - click one "
                            "to extrude it; Delete removes a construction face's lines.\n",
                            added, added == 1 ? "" : "s" );
                m_valid = false;
                g_nUpdateBits = -1;
                return;
            }

            // Modern face selection is absent from selected_brushes; promote its brush
            // before opening undo so the head clones the source and tail stamps halves.
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
        // Read the current face plane and normalize defensively each frame.
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

        // Cached slide axis from the current frame's DeriveBasis.
        const float *OffDir() const { return m_off; }

        // KIWI (2026-10-26): face mode lays construction, so only the region weld bounds
        // it (KSPLIT_CON_MIN_EDGE); brush mode keeps the 1-unit brush-sliver floor.
        float MinSpan() const
        {
            return m_faceOnly ? 2.0f * KSPLIT_CON_MIN_EDGE : KSPLIT_MIN_SPAN;
        }

        // KIWI (2026-10-26): a face too narrow across U seeds on V instead of refusing.
        bool DeriveEitherAxis()
        {
            if ( Derive() )
                return true;
            if ( !m_tooSmall )
                return false;
            m_axis = 1;
            if ( Derive() )
                return true;
            m_axis = 0;
            return false;
        }

        // Rebuild face basis and projected span independently of cut position.
        bool DeriveBasis()
        {
            m_tooSmall = false;
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

            // Longest edge defines U and bounds the preview line.
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

            // U crosses the longest edge direction; V runs along it.
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
            m_tooSmall = !( m_hi - m_lo > MinSpan() );
            if ( m_tooSmall )
                return false;               // a sliver: no room for two halves

            m_half     = radius * 1.05f;
            m_centreT  = Dot3( m_off, c );
            Copy3( c, m_centre );
            return true;
        }

        // Position priority is typed offset, last cursor projection, then centroid.
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
            const float guard = MinSpan() * 0.5f;
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
            // KIWI: face mode shows only the cut across the face itself.
            if ( m_faceOnly )
                ClipLineToFace( pc );
            m_valid = true;
            return true;
        }

        // KIWI: m_a / m_b trimmed to the (convex) face winding along m_lineDir through pc.
        void ClipLineToFace( const float pc[3] )
        {
            const winding_t *w = m_node->def->faces[m_face].w;
            float lo = -m_half, hi = m_half;
            for ( int i = 0; i < w->numpoints; ++i )
            {
                const float *p = w->p[i];
                const float *q = w->p[( i + 1 ) % w->numpoints];
                float e[3], in[3], rel[3];
                Sub3( q, p, e );
                Cross3( m_nrm, e, in );                 // in-plane edge normal
                Sub3( m_centre, p, rel );
                if ( Dot3( in, rel ) < 0.0f )           // point it inward
                    for ( int k = 0; k < 3; ++k )
                        in[k] = -in[k];
                // dot(in, pc + s*dir - p) >= 0
                Sub3( pc, p, rel );
                const float base = Dot3( in, rel );
                const float rate = Dot3( in, m_lineDir );
                if ( fabsf( rate ) < 1.0e-9f )
                    continue;
                const float s = -base / rate;
                if ( rate > 0.0f ) { if ( s > lo ) lo = s; }
                else               { if ( s < hi ) hi = s; }
            }
            if ( hi > lo )
            {
                Mad3( pc, m_lineDir, lo, m_a );
                Mad3( pc, m_lineDir, hi, m_b );
            }
        }

        // KIWI: Sutherland-Hodgman - the part of the face winding on one side of the cut.
        void HalfOfFace( bool front, std::vector<float> &out ) const
        {
            out.clear();
            const winding_t *w = m_node->def->faces[m_face].w;
            const int n = w->numpoints;
            for ( int i = 0; i < n; ++i )
            {
                const float *p = w->p[i];
                const float *q = w->p[( i + 1 ) % n];
                float dp = Dot3( m_planeN, p ) - m_planeD;
                float dq = Dot3( m_planeN, q ) - m_planeD;
                if ( !front ) { dp = -dp; dq = -dq; }
                if ( dp >= 0.0f )
                    out.insert( out.end(), p, p + 3 );
                if ( ( dp >= 0.0f ) != ( dq >= 0.0f ) )
                {
                    const float t = dp / ( dp - dq );
                    for ( int k = 0; k < 3; ++k )
                        out.push_back( p[k] + ( q[k] - p[k] ) * t );
                }
            }
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
                // Display offset with total span so its low-edge reference has scale.
                char offset[48], span[48];
                _snprintf( m_hud, sizeof( m_hud ),
                           "%s  %s  offset %s / %s%s  (Tab flips)",
                           m_faceOnly ? "split face" : "split brush",
                           ( m_axis == 0 ) ? "U" : "V",
                           KiwiFmt_Num( offset, sizeof( offset ),
                                        Units_ToDisplay( m_t - m_lo ), 6 ),
                           KiwiFmt_Num( span, sizeof( span ),
                                        Units_ToDisplay( m_hi - m_lo ), 6 ),
                           m_hasNum ? "  [typed]" : "" );
            }
            m_hud[sizeof( m_hud ) - 1] = '\0';
        }

        selbrush_t *m_node = 0;
        int   m_face   = -1;
        bool  m_faceOnly   = false;        // KIWI: face mode - construction faces, no brush split
        int   m_axis   = 0;                 // 0 = U, 1 = V
        bool  m_valid  = false;
        bool  m_tooSmall = false;           // DeriveBasis: the face spans under KSPLIT_MIN_SPAN
        // Typed offset outranks cursor projection; m_t is the resolved position.
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

    // KIWI (2026-09-28, user: "allow the ctrl-R to be used on construction faces to add a
    // perfect bisecting line in the plane of the face"): Ctrl+R with regions selected lays a
    // construction line across each one, in its plane, clipped to its interior (a ring's line
    // stops at the hole, the even-odd way the region itself is filled).  It seeds at the exact
    // half of each region's span and follows the mouse like face mode (KIWI 2026-10-24, user:
    // "it needs to follow the mouse like normal") - on-plane geometry snap, grid, the half
    // latch within 8 px; Tab flips U/V, a typed number is the offset from the low edge.  The line crosses the
    // region's longest edge (U) or runs along it (V).  Several regions each take their own
    // line at the same fraction of their own span.  ONE construction undo snapshot.
    class KiwiSplitRegionCommand : public KiwiEditorCommand
    {
    public:
        const char *Name() const override { return "Split Region"; }
        bool CanExecute() override { return KiwiRegion_HasSelection(); }
        kiwiSnapCtx_t SnapContext() const override { return KSNAPCTX_CONSTRUCT; }   // Ctrl frees

        int NumericFields( const kiwiNumField_t **out ) const override
        { *out = KSPLITFACE_FIELDS; return 1; }

        bool NumericFieldValue( int field, float *out ) const override
        {
            if ( field != 0 || !out || m_regs.empty() )
                return false;
            const regSplit_t &r = m_regs[0];
            *out = TOf( r ) - r.lo;
            return true;
        }

        bool BubbleAnchor( float *out3 ) const override
        {
            if ( !out3 || m_lines.empty() )
                return false;
            for ( int k = 0; k < 3; ++k )
                out3[k] = ( m_lines[k] + m_lines[3 + k] ) * 0.5f;
            return true;
        }

        void NumericChanged( bool has, float world ) override
        {
            m_hasNum   = has && KiwiNum_HasValue();
            m_numWorld = world;
            Rebuild();
            g_nUpdateBits |= 1;
        }

        const char *HudStatus() const override { return m_hud[0] ? m_hud : 0; }
        bool        HudInvalid() const override { return m_lines.empty(); }

        int HudPrompts( const kiwiPrompt_t **out ) const override
        {
            static const kiwiPrompt_t s_prompts[] = {
                { "Tab",  "Flip U / V" },
                { "Move", "Slide the line" },
                { "Dot",  "Centre - snaps for an exact half" },
                { "0-9",  "Exact offset" },
                { "!",    "Adds the bisecting line across the construction face" },
            };
            *out = s_prompts;
            return (int)( sizeof( s_prompts ) / sizeof( s_prompts[0] ) );
        }

        bool Begin() override
        {
            m_regs.clear();
            m_lines.clear();
            m_axis     = 0;
            m_frac     = 0.5f;
            m_hasNum   = false;
            m_numWorld = 0.0f;
            m_hud[0]   = '\0';

            const std::vector<kregion_t> &all = KiwiRegion_All();
            const int n = KiwiRegion_SelectedCount();
            for ( int i = 0; i < n; ++i )
            {
                const int ri = KiwiRegion_SelectedAt( i );
                if ( ri < 0 || ri >= (int)all.size() )
                    continue;
                const kregion_t &src = all[(size_t)ri];
                regSplit_t r;
                r.plane = src.plane;
                r.loops.push_back( KiwiRegion_OuterLoop( src ) );
                for ( size_t h = 0; h < src.holes.size(); ++h )
                    r.loops.push_back( src.holes[h] );
                if ( !LongestEdge( r.loops[0], r.edge ) )
                    continue;
                m_regs.push_back( r );
            }
            if ( m_regs.empty() )
            {
                Sys_Printf( "Split Region: the selected region is degenerate.\n" );
                return false;
            }

            Rebuild();
            // KIWI (2026-10-26): a region with no room across U seeds on V instead.
            if ( m_lines.empty() )
            {
                m_axis = 1;
                Rebuild();
                if ( m_lines.empty() )
                {
                    m_axis = 0;
                    Rebuild();
                }
            }
            Sys_Printf( "Split Region: %i region(s) - move the mouse to slide the line, Tab "
                        "flips U/V, type an exact offset, RMB / Enter adds it.\n",
                        (int)m_regs.size() );
            return true;
        }

        bool KeyDown( int vk, unsigned mods ) override
        {
            (void)mods;
            if ( vk != 0x09 )                   // VK_TAB
                return false;
            m_axis = ( m_axis + 1 ) & 1;
            m_frac = 0.5f;                      // a new axis re-seats at the half
            Rebuild();
            g_nUpdateBits |= 1;
            return true;
        }

        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick;
            if ( m_regs.empty() )
                return;
            const regSplit_t &r = m_regs[0];
            if ( !( r.hi - r.lo > KSPLITREG_MIN_SPAN ) )
                return;

            float world[3];
            bool  have = false;
            float uv[2], off[2], dir[2];
            Axes( r, dir, off );
            // Geometry snaps from ANY plane project onto the slide axis (see RayAxisClosest).
            const bool tookGeometry = snap.valid && KiwiSnap_IsGeometry( snap.type )
                                   && snap.type != SNAP_FACE;
            if ( tookGeometry )
            {
                Copy3( snap.position, world );
                have = true;
            }
            const bool suppressed = snap.valid && snap.type == SNAP_NONE;
            if ( !have )
            {
                // The slide axis in world space, through the span middle.
                float base[3], axis[3];
                MidWorld( r, base );
                for ( int k = 0; k < 3; ++k )
                    axis[k] = r.plane.u[k] * off[0] + r.plane.v[k] * off[1];
                ray_t ray;
                float hit[3];
                if ( Norm3( axis ) && Pick_RayFromCursor( &ray )
                  && ( RayAxisClosest( ray, base, axis, hit )
                    || KiwiCon_RayPlane( r.plane, ray, hit ) ) )
                {
                    float snapped[3];
                    if ( !suppressed && KiwiGrid_Snap( hit, snapped ) )
                        Copy3( snapped, world );
                    else
                        Copy3( hit, world );
                    have = true;
                }
            }
            if ( !have )
                return;

            KiwiCon_WorldToPlane( r.plane, world, uv );
            const float t = off[0] * uv[0] + off[1] * uv[1];
            m_frac = ( t - r.lo ) / ( r.hi - r.lo );

            // The exact half latches within 8 px unless Ctrl / typing / a geometry snap.
            if ( !suppressed && !tookGeometry && !m_hasNum )
            {
                float mid[3];
                MidWorld( r, mid );
                const float px = KSPLIT_CENTRE_SNAP_PIX * KiwiCam_WorldPerPixel( mid );
                if ( px > 0.0f && fabsf( t - ( r.lo + r.hi ) * 0.5f ) <= px )
                    m_frac = 0.5f;
            }
            Rebuild();
        }

        void DrawWorld() override
        {
            KiwiLines_Color( KSPLIT_EDGE_COL[0], KSPLIT_EDGE_COL[1], KSPLIT_EDGE_COL[2] );
            for ( size_t i = 0; i + 6 <= m_lines.size(); i += 6 )
                if ( !KiwiLines_Add( &m_lines[i], &m_lines[i + 3] ) )
                    break;
            if ( m_regs.empty() )
                return;
            float mid[3];
            MidWorld( m_regs[0], mid );
            KiwiLines_Color( KSPLIT_CENTRE_COL[0], KSPLIT_CENTRE_COL[1], KSPLIT_CENTRE_COL[2] );
            KiwiSnap_EmitSpot( mid, KiwiSnap_AccentPixels() * 2.0f, true  );
            KiwiSnap_EmitSpot( mid, KiwiSnap_RingPixels(),          false );
        }

        int LineBudget() const override
        {
            return (int)( m_lines.size() / 6 ) * 2 + 64;
        }

        void Commit() override
        {
            if ( m_lines.empty() )
            {
                Sys_Printf( "Split Region: nothing to add.\n" );
                return;
            }
            KiwiCon_UndoPush();
            int added = 0;
            size_t li = 0;
            for ( size_t r = 0; r < m_regs.size(); ++r )
                for ( int k = 0; k < m_regs[r].count; ++k, li += 6 )
                {
                    kconObject_t o;
                    o.type       = KCON_LINE;
                    o.plane      = m_regs[r].plane;
                    o.planeValid = true;
                    o.pts.assign( m_lines.begin() + li, m_lines.begin() + li + 6 );
                    if ( KiwiCon_Add( o ) >= 0 )
                        ++added;
                }
            Sys_Printf( "Split Region: %i line(s) added across %i region(s).\n",
                        added, (int)m_regs.size() );
            m_regs.clear();
            m_lines.clear();
            g_nUpdateBits = -1;
        }

        void Cancel() override
        {
            m_regs.clear();
            m_lines.clear();
            g_nUpdateBits |= 1;
        }

    private:
        struct regSplit_t
        {
            kconPlane_t plane;
            std::vector< std::vector<float> > loops;    // outer + holes, plane space
            float edge[2] = { 1.0f, 0.0f };             // longest outer edge, unit
            float lo = 0.0f, hi = 0.0f;                 // span along the slide axis
            int   count = 0;                            // m_lines entries it owns
        };

        static bool LongestEdge( const std::vector<float> &loop, float out[2] )
        {
            const int n = (int)( loop.size() / 2 );
            float best = 0.0f;
            for ( int i = 0; i < n; ++i )
            {
                const int j = ( i + 1 ) % n;
                const float ex = loop[(size_t)j * 2]     - loop[(size_t)i * 2];
                const float ey = loop[(size_t)j * 2 + 1] - loop[(size_t)i * 2 + 1];
                const float l  = sqrtf( ex * ex + ey * ey );
                if ( l > best ) { best = l; out[0] = ex / l; out[1] = ey / l; }
            }
            return best > KSPLIT_EPS;
        }

        // dir = the line's direction, off = the slide axis (both plane space, unit).
        void Axes( const regSplit_t &r, float dir[2], float off[2] ) const
        {
            if ( m_axis == 0 ) { dir[0] = -r.edge[1]; dir[1] = r.edge[0]; }   // crosses it
            else               { dir[0] =  r.edge[0]; dir[1] = r.edge[1]; }   // along it
            off[0] = -dir[1];
            off[1] =  dir[0];
        }

        // The resolved slide position for one region: typed offset, else the fraction.
        float TOf( const regSplit_t &r ) const
        {
            const float guard = KSPLITREG_MIN_SPAN * 0.5f;
            float t = m_hasNum ? ( r.lo + m_numWorld ) : ( r.lo + m_frac * ( r.hi - r.lo ) );
            if ( t < r.lo + guard ) t = r.lo + guard;
            if ( t > r.hi - guard ) t = r.hi - guard;
            return t;
        }

        // World point at the middle of the region's span on the (current) line.
        void MidWorld( const regSplit_t &r, float out[3] ) const
        {
            float dir[2], off[2];
            Axes( r, dir, off );
            float slo = 1e30f, shi = -1e30f;
            const std::vector<float> &L = r.loops[0];
            for ( size_t i = 0; i + 1 < L.size(); i += 2 )
            {
                const float s = dir[0] * L[i] + dir[1] * L[i + 1];
                if ( s < slo ) slo = s;
                if ( s > shi ) shi = s;
            }
            const float t  = ( r.lo + r.hi ) * 0.5f;
            const float s  = ( slo + shi ) * 0.5f;
            const float uv[2] = { off[0] * t + dir[0] * s, off[1] * t + dir[1] * s };
            KiwiCon_PlaneToWorld( r.plane, uv, out );
        }

        // Re-derive every region's span and interior intervals of its line.
        void Rebuild()
        {
            m_lines.clear();
            for ( size_t ri = 0; ri < m_regs.size(); ++ri )
            {
                regSplit_t &r = m_regs[ri];
                r.count = 0;
                float dir[2], off[2];
                Axes( r, dir, off );
                r.lo = 1e30f; r.hi = -1e30f;
                const std::vector<float> &L = r.loops[0];
                for ( size_t i = 0; i + 1 < L.size(); i += 2 )
                {
                    const float t = off[0] * L[i] + off[1] * L[i + 1];
                    if ( t < r.lo ) r.lo = t;
                    if ( t > r.hi ) r.hi = t;
                }
                if ( !( r.hi - r.lo > KSPLITREG_MIN_SPAN ) )
                    continue;                   // a sliver: nothing to split
                const float t = TOf( r );

                // Even-odd crossings of the line off.p == t with every loop edge; the
                // half-open side test counts a vertex exactly on the line once.
                std::vector<float> hits;
                for ( size_t l = 0; l < r.loops.size(); ++l )
                {
                    const std::vector<float> &P = r.loops[l];
                    const int n = (int)( P.size() / 2 );
                    for ( int i = 0; i < n; ++i )
                    {
                        const int j = ( i + 1 ) % n;
                        const float ax = P[(size_t)i * 2], ay = P[(size_t)i * 2 + 1];
                        const float bx = P[(size_t)j * 2], by = P[(size_t)j * 2 + 1];
                        const float da = off[0] * ax + off[1] * ay - t;
                        const float db = off[0] * bx + off[1] * by - t;
                        if ( ( da > 0.0f ) == ( db > 0.0f ) )
                            continue;
                        const float f  = da / ( da - db );
                        const float hx = ax + ( bx - ax ) * f;
                        const float hy = ay + ( by - ay ) * f;
                        hits.push_back( dir[0] * hx + dir[1] * hy );
                    }
                }
                std::sort( hits.begin(), hits.end() );
                for ( size_t h = 0; h + 1 < hits.size(); h += 2 )
                {
                    if ( !( hits[h + 1] - hits[h] > KSPLIT_CON_MIN_EDGE ) )
                        continue;
                    for ( int e = 0; e < 2; ++e )
                    {
                        const float s = hits[h + (size_t)e];
                        const float uv[2] = { off[0] * t + dir[0] * s, off[1] * t + dir[1] * s };
                        float w[3];
                        KiwiCon_PlaneToWorld( r.plane, uv, w );
                        m_lines.push_back( w[0] );
                        m_lines.push_back( w[1] );
                        m_lines.push_back( w[2] );
                    }
                    ++r.count;
                }
            }
            UpdateHud();
        }

        void UpdateHud()
        {
            if ( m_lines.empty() || m_regs.empty() )
            {
                _snprintf( m_hud, sizeof( m_hud ), "split region  %s  (no room to split)",
                           ( m_axis == 0 ) ? "U" : "V" );
            }
            else
            {
                const regSplit_t &r = m_regs[0];
                char offset[48], span[48];
                _snprintf( m_hud, sizeof( m_hud ),
                           "split region  %s  offset %s / %s%s%s  (Tab flips)",
                           ( m_axis == 0 ) ? "U" : "V",
                           KiwiFmt_Num( offset, sizeof( offset ),
                                        Units_ToDisplay( TOf( r ) - r.lo ), 6 ),
                           KiwiFmt_Num( span, sizeof( span ),
                                        Units_ToDisplay( r.hi - r.lo ), 6 ),
                           ( !m_hasNum && m_frac == 0.5f ) ? "  [half]" : "",
                           m_hasNum ? "  [typed]" : "" );
            }
            m_hud[sizeof( m_hud ) - 1] = '\0';
        }

        std::vector<regSplit_t> m_regs;
        std::vector<float>      m_lines;        // world segments, 6 floats each, by region
        int   m_axis     = 0;
        float m_frac     = 0.5f;
        bool  m_hasNum   = false;
        float m_numWorld = 0.0f;
        char  m_hud[160] = { 0 };
    };

    // ── C on construction lines: cut them where other lines cross them ──────────────
    // KIWI (2026-10-05, user: "allow cutting of lines with another line").  Select the lines
    // to cut, press C, click the cutter (Shift+click adds / removes cutters), RMB / Enter
    // cuts: the brush Cut's grammar with lines as the target.  Every selected line a cutter
    // crosses is replaced by its pieces; the cutters stay whole (a selected line clicked as a
    // cutter is not cut), as Plasticity's Cut keeps its cutting curve.
    //   Crossing: 3D closest approach of two segments within KCON_ISECT_DIST, the test Trim,
    //   intersection snapping and the region arrangement use.  Parallel pairs (angle sine
    //   under 1e-3, intersection snapping's cutoff) have no stable crossing: only a cutter
    //   END lying on the span cuts there (an overlapping collinear cutter cuts at its ends).
    //   A cutter END on the target's interior (a T) cuts it, as the arrangement splits
    //   T-junctions; a contact at the TARGET's own open end is a touch, not a cut.
    //   Crossings within KREG_JOIN_DIST of an open end / of each other / of a corner
    //   collapse onto it: the store's normalizer would weld a shorter piece away.
    //   Lines, polylines and rectangles split into LINE / POLYLINE pieces (a rectangle
    //   becomes polylines, as in Trim).  Circles and arcs split into ARCs on the same circle,
    //   solved on the TRUE curve for a straight cutter.  A closed loop needs two crossings
    //   (one leaves it whole); splines are layout curves and are never cut.
    //   ONE construction undo snapshot; the pieces and the untouched selected lines end up
    //   selected, as a brush cut leaves its halves selected.
    const float KCUTL_MERGE = KREG_JOIN_DIST;
    // Angle sine below which two segments are parallel (kiwi_snap.cpp ScanConIntersections).
    const float KCUTL_PARALLEL_SIN = 1.0e-3f;

    enum { KCUTL_UNTOUCHED = 0, KCUTL_CUT, KCUTL_REFUSED };

    struct lcBox_t { float lo[3], hi[3]; };

    void LcBoxSeed( lcBox_t *b, const float *p )
    {
        for ( int k = 0; k < 3; ++k ) { b->lo[k] = p[k]; b->hi[k] = p[k]; }
    }
    void LcBoxAdd( lcBox_t *b, const float *p )
    {
        for ( int k = 0; k < 3; ++k )
        {
            if ( p[k] < b->lo[k] ) b->lo[k] = p[k];
            if ( p[k] > b->hi[k] ) b->hi[k] = p[k];
        }
    }
    void LcBoxGrow( lcBox_t *b, float r )
    {
        for ( int k = 0; k < 3; ++k ) { b->lo[k] -= r; b->hi[k] += r; }
    }
    bool LcBoxOverlap( const lcBox_t &a, const lcBox_t &b )
    {
        for ( int k = 0; k < 3; ++k )
            if ( a.hi[k] < b.lo[k] || b.hi[k] < a.lo[k] )
                return false;
        return true;
    }

    // A target walked as one chain with cumulative WORLD arc length (Trim's chain_t); a
    // closed chain repeats its first point last.
    struct lcChain_t
    {
        std::vector<float> p;           // 3 floats per point
        std::vector<float> cum;         // arc length per point; cum[0] == 0
        bool               closed = false;

        int   Count() const { return (int)( p.size() / 3 ); }
        float Total() const { return cum.empty() ? 0.0f : cum[cum.size() - 1]; }
    };

    bool LcBuildChain( const kconObject_t &o, lcChain_t *c )
    {
        c->p.clear();
        c->cum.clear();
        // The store's closed rule (KiwiCon_SegmentCount).
        c->closed = ( o.type == KCON_CIRCLE ) || ( o.type == KCON_RECT ) || o.closed;
        const int n = KiwiCon_VertCount( o );
        if ( n < 2 )
            return false;
        for ( int i = 0; i < n; ++i )
        {
            float w[3];
            if ( !KiwiCon_VertWorld( o, i, w ) )
                return false;
            c->p.push_back( w[0] );  c->p.push_back( w[1] );  c->p.push_back( w[2] );
        }
        if ( c->closed )
        {
            const float w0[3] = { c->p[0], c->p[1], c->p[2] };   // copy: push_back may move p
            c->p.push_back( w0[0] );  c->p.push_back( w0[1] );  c->p.push_back( w0[2] );
        }
        const int m = c->Count();
        c->cum.push_back( 0.0f );
        for ( int i = 0; i + 1 < m; ++i )
        {
            float d[3];
            Sub3( &c->p[( (size_t)i + 1 ) * 3], &c->p[(size_t)i * 3], d );
            c->cum.push_back( c->cum[i] + Len3( d ) );
        }
        return c->Total() > 1.0e-4f;
    }

    void LcPointAt( const lcChain_t &c, float s, float out[3] )
    {
        const int m = c.Count();
        if ( s <= 0.0f )      { Copy3( &c.p[0], out ); return; }
        if ( s >= c.Total() ) { Copy3( &c.p[( (size_t)m - 1 ) * 3], out ); return; }
        for ( int i = 0; i + 1 < m; ++i )
        {
            if ( s > c.cum[i + 1] )
                continue;
            const float span = c.cum[i + 1] - c.cum[i];
            const float t    = ( span > 1.0e-6f ) ? ( ( s - c.cum[i] ) / span ) : 0.0f;
            for ( int k = 0; k < 3; ++k )
                out[k] = c.p[(size_t)i * 3 + k]
                       + ( c.p[( (size_t)i + 1 ) * 3 + k] - c.p[(size_t)i * 3 + k] ) * t;
            return;
        }
        Copy3( &c.p[( (size_t)m - 1 ) * 3], out );
    }

    // [a, b] of the chain: interpolated ends plus the original corners between them.
    void LcExtract( const lcChain_t &c, float a, float b, std::vector<float> *out )
    {
        out->clear();
        if ( !( b > a + 1.0e-4f ) )
            return;
        float w[3];
        LcPointAt( c, a, w );
        out->push_back( w[0] );  out->push_back( w[1] );  out->push_back( w[2] );
        const int m = c.Count();
        for ( int i = 0; i < m; ++i )
        {
            if ( c.cum[i] <= a + 1.0e-4f || c.cum[i] >= b - 1.0e-4f )
                continue;
            out->push_back( c.p[(size_t)i * 3 + 0] );
            out->push_back( c.p[(size_t)i * 3 + 1] );
            out->push_back( c.p[(size_t)i * 3 + 2] );
        }
        LcPointAt( c, b, w );
        out->push_back( w[0] );  out->push_back( w[1] );  out->push_back( w[2] );
    }

    // The store's normalizer, dry (consecutive points within KREG_JOIN_DIST weld): a piece
    // KiwiCon_Add would refuse is caught here, BEFORE the undo snapshot.
    bool LcPieceOk( const std::vector<float> &pts )
    {
        const int    n    = (int)( pts.size() / 3 );
        const float *last = 0;
        int          kept = 0;
        for ( int i = 0; i < n; ++i )
        {
            const float *p = &pts[(size_t)i * 3];
            if ( last )
            {
                float d[3];
                Sub3( p, last, d );
                if ( Len3( d ) <= KREG_JOIN_DIST )
                    continue;
            }
            last = p;
            ++kept;
        }
        return kept >= 2 && kept <= KCON_MAX_POINTS;
    }

    // Sorted values: one within `tol` of the last kept value joins it.
    void LcMergeSorted( std::vector<float> *v, float tol )
    {
        std::vector<float> out;
        for ( size_t i = 0; i < v->size(); ++i )
            if ( out.empty() || ( *v )[i] - out.back() > tol )
                out.push_back( ( *v )[i] );
        v->swap( out );
    }

    // A world point's angle about a circle / arc centre, degrees, in its plane.
    float LcAngleOf( const kconObject_t &curve, const float w[3] )
    {
        float uv[2];
        KiwiCon_WorldToPlane( curve.plane, w, uv );
        return atan2f( uv[1] - curve.centre[1], uv[0] - curve.centre[0] ) * KCON_RAD2DEG;
    }

    void LcCurvePoint( const kconObject_t &curve, float deg, float out[3] )
    {
        const float rad   = deg * KCON_DEG2RAD;
        const float uv[2] = { curve.centre[0] + cosf( rad ) * curve.radius,
                              curve.centre[1] + sinf( rad ) * curve.radius };
        KiwiCon_PlaneToWorld( curve.plane, uv, out );
    }

    // A straight segment's crossing with a circle / arc, solved on the TRUE curve: the
    // tessellation only finds it (a chord runs up to r(1 - cos(pi/n)) inside the circle).
    // The segment is projected into the curve's plane and the root nearest `approx` wins,
    // within a tenth of the radius.  Out: the SEGMENT parameter and the curve angle.
    // False keeps the tessellated estimate (a segment along the normal, no root near).
    bool LcRefineOnCurve( const kconObject_t &curve, const float a[3], const float b[3],
                          const float approx[3], float *outT, float *outDeg )
    {
        float ua[2], ub[2];
        KiwiCon_WorldToPlane( curve.plane, a, ua );
        KiwiCon_WorldToPlane( curve.plane, b, ub );
        // Doubles: the discriminant subtracts two large squares.
        const double dx = (double)ub[0] - ua[0], dy = (double)ub[1] - ua[1];
        const double aa = dx * dx + dy * dy;
        if ( aa < 1.0e-6 )
            return false;
        const double fx   = (double)ua[0] - curve.centre[0];
        const double fy   = (double)ua[1] - curve.centre[1];
        const double bb   = dx * fx + dy * fy;
        const double cc   = fx * fx + fy * fy - (double)curve.radius * curve.radius;
        const double disc = bb * bb - aa * cc;
        double roots[2];
        int    nr = 0;
        if ( disc < 0.0 )
            roots[nr++] = -bb / aa;             // grazing: the closest approach
        else
        {
            const double sq = sqrt( disc );
            roots[nr++] = ( -bb - sq ) / aa;
            roots[nr++] = ( -bb + sq ) / aa;
        }
        float e[3];
        Sub3( b, a, e );
        float bestT = 0.0f, bestD = 0.0f;
        bool  have  = false;
        for ( int i = 0; i < nr; ++i )
        {
            float t = (float)roots[i];
            if ( t < 0.0f ) t = 0.0f;
            if ( t > 1.0f ) t = 1.0f;
            float p[3], d[3];
            Mad3( a, e, t, p );
            Sub3( p, approx, d );
            const float dist = Len3( d );
            if ( !have || dist < bestD ) { have = true; bestD = dist; bestT = t; }
        }
        if ( !have || bestD > (std::max)( 1.0f, curve.radius * 0.1f ) )
            return false;
        const double u = ua[0] + dx * bestT - curve.centre[0];
        const double v = ua[1] + dy * bestT - curve.centre[1];
        *outT   = bestT;
        *outDeg = (float)( atan2( v, u ) * KCON_RAD2DEG );
        return true;
    }

    // One cutter: its tessellated WORLD segments and their bounds grown by the tolerance.
    struct lcCutter_t
    {
        int                object = -1;
        bool               open   = true;   // has real ends (first / last segment)
        std::vector<float> seg;             // 6 floats per segment
        lcBox_t            box;
    };

    bool LcBuildCutter( int object, lcCutter_t *out )
    {
        const kconObject_t *o = KiwiCon_At( object );
        if ( !o || o->hidden )
            return false;
        out->object = object;
        out->open   = !( ( o->type == KCON_CIRCLE ) || ( o->type == KCON_RECT ) || o->closed );
        out->seg.clear();
        const int segs = KiwiCon_SegmentCount( *o );
        for ( int s = 0; s < segs; ++s )
        {
            float a[3], b[3];
            if ( !KiwiCon_SegmentWorld( *o, s, a, b ) )
                break;
            for ( int k = 0; k < 3; ++k ) out->seg.push_back( a[k] );
            for ( int k = 0; k < 3; ++k ) out->seg.push_back( b[k] );
        }
        if ( out->seg.empty() )
            return false;
        LcBoxSeed( &out->box, &out->seg[0] );
        for ( size_t i = 3; i + 2 < out->seg.size(); i += 3 )
            LcBoxAdd( &out->box, &out->seg[i] );
        LcBoxGrow( &out->box, KCON_ISECT_DIST );
        return true;
    }

    // One contact: arc length along the chain and, for a curve target, its angle.
    void LcPushCut( const kconObject_t &t, float s, const float at[3], bool haveDeg, float deg,
                    std::vector<float> *outS, std::vector<float> *outDeg )
    {
        outS->push_back( s );
        if ( KiwiCon_IsParametric( t ) )
            outDeg->push_back( haveDeg ? deg : LcAngleOf( t, at ) );
    }

    // Every contact of the target's chain with the cutters (rules: the block comment above).
    void LcGatherCuts( const kconObject_t &t, const lcChain_t &c,
                       const std::vector<lcCutter_t> &cutters,
                       std::vector<float> *outS, std::vector<float> *outDeg )
    {
        outS->clear();
        outDeg->clear();
        const bool curve = KiwiCon_IsParametric( t );
        const int  m     = c.Count();
        lcBox_t chainBox;
        LcBoxSeed( &chainBox, &c.p[0] );
        for ( int k = 1; k < m; ++k )
            LcBoxAdd( &chainBox, &c.p[(size_t)k * 3] );
        LcBoxGrow( &chainBox, KCON_ISECT_DIST );

        for ( size_t ci = 0; ci < cutters.size(); ++ci )
        {
            const lcCutter_t &cut = cutters[ci];
            if ( !LcBoxOverlap( chainBox, cut.box ) )
                continue;
            const kconObject_t *co = KiwiCon_At( cut.object );
            const bool cutCurve = co && KiwiCon_IsParametric( *co );
            const int  ns = (int)( cut.seg.size() / 6 );
            for ( int j = 0; j < ns; ++j )
            {
                const float *wa = &cut.seg[(size_t)j * 6];
                const float *wb = wa + 3;
                lcBox_t sb;
                LcBoxSeed( &sb, wa );
                LcBoxAdd ( &sb, wb );
                LcBoxGrow( &sb, KCON_ISECT_DIST );
                if ( !LcBoxOverlap( chainBox, sb ) )
                    continue;
                float e1[3];
                Sub3( wb, wa, e1 );
                const float l1 = Len3( e1 );
                if ( l1 < 1.0e-4f )
                    continue;

                for ( int k = 0; k + 1 < m; ++k )
                {
                    const float *p0 = &c.p[(size_t)k * 3];
                    const float *p1 = p0 + 3;
                    lcBox_t kb;
                    LcBoxSeed( &kb, p0 );
                    LcBoxAdd ( &kb, p1 );
                    if ( !LcBoxOverlap( sb, kb ) )
                        continue;
                    float e0[3];
                    Sub3( p1, p0, e0 );
                    const float l0 = Len3( e0 );
                    if ( l0 < 1.0e-4f )
                        continue;
                    const float span = c.cum[k + 1] - c.cum[k];

                    float ta = 0.0f, tb = 0.0f;
                    const float d = KiwiCon_SegSegClosest( p0, p1, wa, wb, &ta, &tb, 0 );
                    if ( d > KCON_ISECT_DIST )
                        continue;

                    float cr[3];
                    Cross3( e0, e1, cr );
                    if ( Len3( cr ) < KCUTL_PARALLEL_SIN * l0 * l1 )
                    {
                        // Parallel: only the cutter's own ENDS that sit on this span.  A vertex
                        // where it turns away is caught by the next (non-parallel) segment.
                        const float *ends[2]  = { wa, wb };
                        const bool   isEnd[2] = { cut.open && j == 0, cut.open && j == ns - 1 };
                        for ( int q = 0; q < 2; ++q )
                        {
                            if ( !isEnd[q] )
                                continue;
                            float rel[3], foot[3], off[3];
                            Sub3( ends[q], p0, rel );
                            const float tt = Dot3( rel, e0 ) / ( l0 * l0 );
                            if ( tt < 0.0f || tt > 1.0f )
                                continue;
                            Mad3( p0, e0, tt, foot );
                            Sub3( ends[q], foot, off );
                            if ( Len3( off ) > KCON_ISECT_DIST )
                                continue;
                            LcPushCut( t, c.cum[k] + span * tt, foot, false, 0.0f, outS, outDeg );
                        }
                        continue;
                    }

                    float at[3];
                    Mad3( p0, e0, ta, at );
                    float s = c.cum[k] + span * ta;
                    float deg = 0.0f, rt = 0.0f;
                    bool  haveDeg = false;
                    if ( curve && !cutCurve )
                        haveDeg = LcRefineOnCurve( t, wa, wb, at, &rt, &deg );
                    else if ( !curve && cutCurve )
                    {
                        float cutterDeg = 0.0f;
                        if ( LcRefineOnCurve( *co, p0, p1, at, &rt, &cutterDeg ) )
                        {
                            s = c.cum[k] + span * rt;
                            Mad3( p0, e0, rt, at );
                        }
                    }
                    LcPushCut( t, s, at, haveDeg, deg, outS, outDeg );
                }
            }
        }
    }

    // Stored-point targets (line / polyline / rectangle; a spline's tessellation is only
    // walked to report it).  The pieces are LINE / POLYLINE objects.
    int LcPlanChain( const kconObject_t &t, const lcChain_t &c, const std::vector<float> &raw,
                     std::vector<kconObject_t> *pieces, std::vector<float> *points,
                     const char **why )
    {
        const float total = c.Total();
        const int   m     = c.Count();
        std::vector<float> cuts;
        for ( size_t i = 0; i < raw.size(); ++i )
        {
            float s = raw[i];
            // A crossing this close to a corner IS the corner (the store welds the sliver).
            for ( int k = 0; k < m; ++k )
                if ( fabsf( s - c.cum[k] ) <= KCUTL_MERGE ) { s = c.cum[k]; break; }
            if ( c.closed )
            {
                if ( s >= total - 1.0e-4f )
                    s = 0.0f;                   // the seam is the first point
            }
            else if ( s <= KCUTL_MERGE || s >= total - KCUTL_MERGE )
                continue;                       // its own end: a touch, not a cut
            cuts.push_back( s );
        }
        std::sort( cuts.begin(), cuts.end() );
        LcMergeSorted( &cuts, KCUTL_MERGE );
        if ( c.closed && cuts.size() >= 2 && cuts[0] + total - cuts.back() <= KCUTL_MERGE )
            cuts.pop_back();                    // one point either side of the seam
        if ( cuts.empty() )
            return KCUTL_UNTOUCHED;
        if ( c.closed && cuts.size() < 2 )
        {
            *why = "a closed loop needs TWO crossings to come apart";
            return KCUTL_REFUSED;
        }

        std::vector< std::vector<float> > runs;
        if ( c.closed )
        {
            for ( size_t i = 0; i + 1 < cuts.size(); ++i )
            {
                runs.push_back( std::vector<float>() );
                LcExtract( c, cuts[i], cuts[i + 1], &runs.back() );
            }
            // The last piece runs from the last cut over the seam to the first.
            std::vector<float> tail, head;
            LcExtract( c, cuts.back(), total, &tail );
            LcExtract( c, 0.0f, cuts[0], &head );
            const size_t first = tail.empty() ? 0u : 3u;     // the seam point once
            for ( size_t k = first; k + 2 < head.size(); k += 3 )
            {
                tail.push_back( head[k] );
                tail.push_back( head[k + 1] );
                tail.push_back( head[k + 2] );
            }
            runs.push_back( tail );
        }
        else
        {
            float lo = 0.0f;
            for ( size_t i = 0; i <= cuts.size(); ++i )
            {
                const float hi = ( i < cuts.size() ) ? cuts[i] : total;
                runs.push_back( std::vector<float>() );
                LcExtract( c, lo, hi, &runs.back() );
                lo = hi;
            }
        }

        for ( size_t r = 0; r < runs.size(); ++r )
        {
            if ( !LcPieceOk( runs[r] ) )
            {
                *why = "a piece would be under the weld distance or over the point limit";
                pieces->clear();
                return KCUTL_REFUSED;
            }
            kconObject_t o;
            o.type   = ( (int)( runs[r].size() / 3 ) == 2 ) ? KCON_LINE : KCON_POLYLINE;
            o.plane  = t.plane;                 // seed only; KiwiCon_Add refits
            o.pts    = runs[r];
            o.closed = false;
            o.group  = t.group;
            o.name   = t.name;
            pieces->push_back( o );
        }
        for ( size_t i = 0; i < cuts.size(); ++i )
        {
            float w[3];
            LcPointAt( c, cuts[i], w );
            points->push_back( w[0] );  points->push_back( w[1] );  points->push_back( w[2] );
        }
        return KCUTL_CUT;
    }

    // Circle / arc targets stay curves: every piece is an ARC on the same circle.  `raw`
    // holds the crossing angles (degrees, plane space).
    int LcPlanCurve( const kconObject_t &t, const std::vector<float> &raw,
                     std::vector<kconObject_t> *pieces, std::vector<float> *points,
                     const char **why )
    {
        if ( !( t.radius > 1.0e-3f ) )
            return KCUTL_UNTOUCHED;
        const float tolDeg = ( KCUTL_MERGE / t.radius ) * KCON_RAD2DEG;   // the weld as an angle
        // Piece bounds as absolute angles, in the curve's own direction.
        std::vector<float> bounds;
        if ( t.type == KCON_CIRCLE )
        {
            std::vector<float> a;
            for ( size_t i = 0; i < raw.size(); ++i )
                a.push_back( KiwiCon_WrapDeg( raw[i] ) );
            std::sort( a.begin(), a.end() );
            LcMergeSorted( &a, tolDeg );
            if ( a.size() >= 2 && a[0] + 360.0f - a.back() <= tolDeg )
                a.pop_back();                   // one point either side of 0 degrees
            if ( a.empty() )
                return KCUTL_UNTOUCHED;
            if ( a.size() < 2 )
            {
                *why = "a closed loop needs TWO crossings to come apart";
                return KCUTL_REFUSED;
            }
            bounds = a;
            bounds.push_back( a[0] + 360.0f );  // the last piece runs over 0 degrees
        }
        else
        {
            const float d     = t.ang1 - t.ang0;
            const float sweep = fabsf( d );
            if ( !( sweep > 1.0e-3f ) )
                return KCUTL_UNTOUCHED;
            const float sgn = ( d < 0.0f ) ? -1.0f : 1.0f;
            std::vector<float> f;               // degrees from ang0, along the sweep
            for ( size_t i = 0; i < raw.size(); ++i )
            {
                float rel = KiwiCon_WrapDeg( ( raw[i] - t.ang0 ) * sgn );
                if ( rel >= 360.0f - tolDeg )
                    rel -= 360.0f;              // just before the start IS the start
                if ( rel <= tolDeg || rel >= sweep - tolDeg )
                    continue;                   // its own end: a touch, not a cut
                f.push_back( rel );
            }
            std::sort( f.begin(), f.end() );
            LcMergeSorted( &f, tolDeg );
            if ( f.empty() )
                return KCUTL_UNTOUCHED;
            bounds.push_back( t.ang0 );
            for ( size_t i = 0; i < f.size(); ++i )
                bounds.push_back( t.ang0 + sgn * f[i] );
            bounds.push_back( t.ang1 );
        }

        for ( size_t i = 0; i + 1 < bounds.size(); ++i )
        {
            kconObject_t o;
            o.type       = KCON_ARC;
            o.plane      = t.plane;
            o.planeValid = true;
            o.centre[0]  = t.centre[0];
            o.centre[1]  = t.centre[1];
            o.radius     = t.radius;
            o.ang0       = bounds[i];
            o.ang1       = bounds[i + 1];
            // A circle's pieces take the Arc tool's radius-driven density; an arc keeps its own.
            o.segs       = ( t.type == KCON_CIRCLE ) ? 0 : t.segs;
            o.closed     = false;
            o.group      = t.group;
            o.name       = t.name;
            pieces->push_back( o );
        }
        // Cut points: every bound but the arc's own ends (a circle's last repeats its first).
        for ( size_t i = ( t.type == KCON_CIRCLE ) ? 0u : 1u; i + 1 < bounds.size(); ++i )
        {
            float w[3];
            LcCurvePoint( t, bounds[i], w );
            points->push_back( w[0] );  points->push_back( w[1] );  points->push_back( w[2] );
        }
        return KCUTL_CUT;
    }

    // One target against the cutters; KCUTL_CUT fills its pieces and cut points.
    int LcPlanTarget( int object, const std::vector<lcCutter_t> &cutters,
                      std::vector<kconObject_t> *pieces, std::vector<float> *points,
                      const char **why )
    {
        pieces->clear();
        points->clear();
        const kconObject_t *t = KiwiCon_At( object );
        if ( !t || t->hidden )
            return KCUTL_UNTOUCHED;
        lcChain_t c;
        if ( !LcBuildChain( *t, &c ) )
            return KCUTL_UNTOUCHED;
        std::vector<float> s, deg;
        LcGatherCuts( *t, c, cutters, &s, &deg );
        if ( s.empty() )
            return KCUTL_UNTOUCHED;
        const int r = KiwiCon_IsParametric( *t ) ? LcPlanCurve( *t, deg, pieces, points, why )
                                                 : LcPlanChain( *t, c, s, pieces, points, why );
        if ( r != KCUTL_UNTOUCHED && KiwiCon_HasSmooth( *t ) )
        {
            // Walked only to say so: a piece could not keep the spline's curve.
            pieces->clear();
            points->clear();
            *why = "it is a spline (a layout curve, never cut; it can still BE the cutter)";
            return KCUTL_REFUSED;
        }
        return r;
    }

    struct lcPlan_t
    {
        int                       object = -1;
        std::vector<kconObject_t> pieces;   // replace `object`, ready for KiwiCon_Add
        std::vector<float>        points;   // the cut points, 3 floats each (preview)
    };

    class KiwiCutLinesCommand : public KiwiEditorCommand
    {
    public:
        const char *Name() const override { return "Cut"; }
        bool CanExecute() override { return CanCutLines(); }

        // Clicks pick cutters in both stages; RMB / Enter commits.
        bool WantsClicks() const override { return true; }
        // Pick-only: nothing is placed, so no snap marker.
        bool ShowsSnap() const override { return false; }

        const char *HudStatus() const override { return m_hud[0] ? m_hud : 0; }
        bool        HudInvalid() const override
        { return m_stage == KCUTL_PREVIEW && m_crossings <= 0; }

        int LineBudget() const override
        {
            int n = 64 + m_crossings * 24;      // ring + dot per cut point
            for ( size_t i = 0; i < m_cutters.size(); ++i )
                if ( const kconObject_t *o = KiwiCon_At( m_cutters[i] ) )
                    n += KiwiCon_SegmentCount( *o );
            if ( m_hover >= 0 )
                if ( const kconObject_t *o = KiwiCon_At( m_hover ) )
                    n += KiwiCon_SegmentCount( *o );
            return n;
        }

        int HudPrompts( const kiwiPrompt_t **out ) const override
        {
            static const kiwiPrompt_t s_pick[] = {
                { "LMB",       "Pick the line to cut with" },
                { "Shift+LMB", "Add a cutter" },
            };
            static const kiwiPrompt_t s_preview[] = {
                { "LMB",       "Cut with this line instead" },
                { "Shift+LMB", "Add / remove a cutter" },
                { "Esc",       "Pick again" },
            };
            if ( m_stage == KCUTL_PICK )
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
            LineCutTargets( &m_targets );
            if ( m_targets.empty() )
            {
                Sys_Printf( "Cut: select a brush, or the construction lines to cut.\n" );
                return false;
            }
            m_gen = KiwiCon_Generation();
            UpdateHud();
            Sys_Printf( "Cut: click the line to cut the %i selected line(s) with — "
                        "Shift+click adds cutters, RMB / Enter cuts.  The cutter stays "
                        "whole.\n", (int)m_targets.size() );
            return true;
        }

        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick;  (void)snap;
            const int was = m_hover;
            int x = 0, y = 0, obj = -1;
            m_hover = -1;
            if ( KiwiCmd_LastCursor( &x, &y )
              && KiwiCon_PickSegmentAt( x, y, 0, 0, &obj, 0, 0 ) && obj >= 0 )
                m_hover = obj;
            if ( m_hover != was )
            {
                UpdateHud();
                g_nUpdateBits |= 1;
            }
        }

        // A bare click makes the hovered shape the only cutter; Shift toggles it.
        bool Click() override
        {
            if ( m_hover < 0 )
            {
                Sys_Printf( "Cut: nothing to cut with there — click a construction line, "
                            "or press Esc.\n" );
                return true;
            }
            if ( !KiwiCmd_LastShift() )
                m_cutters.clear();
            bool removed = false;
            for ( size_t i = 0; i < m_cutters.size(); )
            {
                if ( m_cutters[i] == m_hover )
                {
                    m_cutters.erase( m_cutters.begin() + i );
                    removed = true;
                    continue;
                }
                ++i;
            }
            if ( !removed )
                m_cutters.push_back( m_hover );

            if ( m_cutters.empty() )
            {
                m_stage = KCUTL_PICK;
                ClearPlan();
                UpdateHud();
                Sys_Printf( "Cut: no cutter left — click the line to cut with.\n" );
                g_nUpdateBits |= 1;
                return true;
            }

            m_stage = KCUTL_PREVIEW;
            Rebuild( true );
            if ( !removed && IsTarget( m_hover ) )
                Sys_Printf( "Cut: that line was selected too — as a cutter it stays whole.\n" );
            if ( m_crossings > 0 )
                Sys_Printf( "Cut: %i crossing(s) on %i line(s) — RMB / Enter cuts, "
                            "Shift+click adds a cutter, Esc picks again.\n",
                            m_crossings, (int)m_plans.size() );
            else if ( m_refused == 0 )
                Sys_Printf( "Cut: %s none of the selected lines — click another, or Esc.\n",
                            ( m_cutters.size() == 1 ) ? "that line crosses"
                                                      : "the cutters cross" );
            g_nUpdateBits |= 1;
            return true;
        }

        // Esc steps the preview back to picking; Enter is consumed until a cutter exists.
        bool KeyDown( int vk, unsigned mods ) override
        {
            (void)mods;
            if ( vk == 0x1B && m_stage == KCUTL_PREVIEW )    // VK_ESCAPE
            {
                m_stage = KCUTL_PICK;
                m_cutters.clear();
                ClearPlan();
                UpdateHud();
                Sys_Printf( "Cut: cutters dropped — click another line (Esc again "
                            "cancels).\n" );
                g_nUpdateBits |= 1;
                return true;
            }
            if ( vk == 0x0D && m_stage == KCUTL_PICK )       // VK_RETURN
            {
                Sys_Printf( "Cut: click the line to cut with first.\n" );
                return true;
            }
            return false;
        }

        void DrawWorld() override
        {
            KiwiLines_Color( KSPLIT_EDGE_COL[0], KSPLIT_EDGE_COL[1], KSPLIT_EDGE_COL[2] );
            for ( size_t i = 0; i < m_cutters.size(); ++i )
                if ( !DrawShape( m_cutters[i] ) )
                    return;
            if ( m_hover >= 0 && !IsCutter( m_hover ) )
            {
                KiwiLines_Color( KSPLIT_HOT_COL[0], KSPLIT_HOT_COL[1], KSPLIT_HOT_COL[2] );
                if ( !DrawShape( m_hover ) )
                    return;
            }
            // Every split point: a ring with a dot, in the cut-outline red.
            KiwiLines_Color( KSPLIT_LINE_COL[0], KSPLIT_LINE_COL[1], KSPLIT_LINE_COL[2] );
            for ( size_t p = 0; p < m_plans.size(); ++p )
                for ( size_t i = 0; i + 2 < m_plans[p].points.size(); i += 3 )
                {
                    KiwiSnap_EmitSpot( &m_plans[p].points[i], KiwiSnap_RingPixels(), false );
                    KiwiSnap_EmitSpot( &m_plans[p].points[i], KiwiSnap_AccentPixels() * 2.0f, true );
                }
        }

        void Commit() override
        {
            if ( m_stage != KCUTL_PREVIEW || m_cutters.empty() )
            {
                Sys_Printf( "Cut: no cutter was picked — nothing was cut.\n" );
                Reset();
                return;
            }
            if ( KiwiCon_Generation() != m_gen )
            {
                Sys_Printf( "Cut: the construction changed while Cut was open — nothing "
                            "was cut.  Press C again.\n" );
                Reset();
                return;
            }
            Rebuild( false );                   // the previewed plan, from the live store
            if ( m_plans.empty() )
            {
                Sys_Printf( "Cut: nothing is crossed — nothing was cut.\n" );
                Reset();
                return;
            }

            std::vector<int> gone;
            for ( size_t i = 0; i < m_plans.size(); ++i )
                gone.push_back( m_plans[i].object );
            std::sort( gone.begin(), gone.end() );

            // Selected lines the cut left alone stay selected beside the pieces; each index
            // drops by the removals below it.
            std::vector<int> sel;
            for ( size_t i = 0; i < m_targets.size(); ++i )
            {
                const int t = m_targets[i];
                if ( std::binary_search( gone.begin(), gone.end(), t ) )
                    continue;
                const int below = (int)( std::lower_bound( gone.begin(), gone.end(), t )
                                       - gone.begin() );
                sel.push_back( t - below );
            }

            // Every fallible step is behind us: ONE snapshot, then the plan completes.
            KiwiCon_UndoPush();
            for ( int i = (int)gone.size() - 1; i >= 0; --i )
                KiwiCon_RemoveAt( gone[(size_t)i] );       // high first: lower indices hold
            int made = 0;
            for ( size_t p = 0; p < m_plans.size(); ++p )
                for ( size_t k = 0; k < m_plans[p].pieces.size(); ++k )
                {
                    const int idx = KiwiCon_Add( m_plans[p].pieces[k] );
                    if ( idx >= 0 )
                    {
                        sel.push_back( idx );
                        ++made;
                    }
                }
            KiwiConSel_SelectObjects( sel.empty() ? 0 : &sel[0], (int)sel.size() );

            Sys_Printf( "Cut: %i line(s) -> %i piece(s) at %i crossing(s); the cutter%s "
                        "stayed whole.\n", (int)gone.size(), made, m_crossings,
                        ( m_cutters.size() == 1 ) ? "" : "s" );
            Reset();
            g_nUpdateBits = -1;
        }

        void Cancel() override
        {
            Reset();
            g_nUpdateBits |= 1;
        }

    private:
        void ClearPlan()
        {
            m_plans.clear();
            m_crossings = 0;
            m_refused   = 0;
        }

        void Reset()
        {
            m_stage = KCUTL_PICK;
            m_targets.clear();
            m_cutters.clear();
            ClearPlan();
            m_hover  = -1;
            m_gen    = 0;
            m_hud[0] = '\0';
        }

        bool IsCutter( int obj ) const
        {
            return std::find( m_cutters.begin(), m_cutters.end(), obj ) != m_cutters.end();
        }

        bool IsTarget( int obj ) const
        {
            return std::binary_search( m_targets.begin(), m_targets.end(), obj );
        }

        static bool DrawShape( int object )
        {
            const kconObject_t *o = KiwiCon_At( object );
            if ( !o )
                return true;
            const int segs = KiwiCon_SegmentCount( *o );
            for ( int s = 0; s < segs; ++s )
            {
                float a[3], b[3];
                if ( !KiwiCon_SegmentWorld( *o, s, a, b ) )
                    break;
                if ( !KiwiLines_Add( a, b ) )
                    return false;
            }
            return true;
        }

        // Re-plan every target against the cutters from the LIVE store.  `report` prints
        // each refusal once (a click), never per frame.
        void Rebuild( bool report )
        {
            ClearPlan();
            if ( KiwiCon_Generation() != m_gen )
            {
                // The latched indices may name other objects now; read none of them.
                if ( report )
                    Sys_Printf( "Cut: the construction changed while Cut was open — Esc, "
                                "then press C again.\n" );
                UpdateHud();
                return;
            }
            std::vector<lcCutter_t> cutters;
            for ( size_t i = 0; i < m_cutters.size(); ++i )
            {
                lcCutter_t cu;
                if ( LcBuildCutter( m_cutters[i], &cu ) )
                    cutters.push_back( cu );
            }
            for ( size_t i = 0; i < m_targets.size() && !cutters.empty(); ++i )
            {
                if ( IsCutter( m_targets[i] ) )
                    continue;                   // a cutter is never cut
                lcPlan_t    plan;
                const char *why = "unknown";
                plan.object = m_targets[i];
                const int r = LcPlanTarget( m_targets[i], cutters, &plan.pieces,
                                            &plan.points, &why );
                if ( r == KCUTL_CUT )
                {
                    m_crossings += (int)( plan.points.size() / 3 );
                    m_plans.push_back( plan );
                }
                else if ( r == KCUTL_REFUSED )
                {
                    ++m_refused;
                    if ( report )
                        Sys_Printf( "Cut: one line left whole — %s.\n", why );
                }
            }
            UpdateHud();
        }

        void UpdateHud()
        {
            if ( m_stage == KCUTL_PICK )
            {
                const char *what = "click the line to cut them with";
                if ( m_hover >= 0 )
                    what = IsTarget( m_hover ) ? "click this SHAPE — it cuts the rest, not itself"
                                               : "click this SHAPE — it cuts, it is not cut";
                _snprintf( m_hud, sizeof( m_hud ), "cut  %i line(s)  %s",
                           (int)m_targets.size(), what );
            }
            else
            {
                int live = 0;                   // targets that are not cutters
                for ( size_t i = 0; i < m_targets.size(); ++i )
                    if ( !IsCutter( m_targets[i] ) )
                        ++live;
                char refused[40] = { 0 };
                if ( m_refused > 0 )
                    _snprintf( refused, sizeof( refused ), "  %i left whole", m_refused );
                refused[sizeof( refused ) - 1] = '\0';
                _snprintf( m_hud, sizeof( m_hud ),
                           "cut  %i crossing(s) on %i of %i line(s)%s  (%i cutter(s) stay whole)",
                           m_crossings, (int)m_plans.size(), live, refused,
                           (int)m_cutters.size() );
            }
            m_hud[sizeof( m_hud ) - 1] = '\0';
        }

        enum stage_t { KCUTL_PICK = 0, KCUTL_PREVIEW };
        stage_t               m_stage = KCUTL_PICK;
        std::vector<int>      m_targets;        // selected objects, ascending (Begin)
        std::vector<int>      m_cutters;        // picked cutters, in pick order
        std::vector<lcPlan_t> m_plans;          // crossed targets and their pieces
        int                   m_hover     = -1; // construction object under the cursor
        int                   m_crossings = 0;
        int                   m_refused   = 0;  // crossed but left whole (and why: console)
        unsigned              m_gen       = 0;  // store generation the indices belong to
        char                  m_hud[160]  = { 0 };
    };

    KiwiCutCommand         s_cut;
    KiwiCutLinesCommand    s_cutLines;
    KiwiSplitFaceCommand   s_splitFace;
    KiwiSplitRegionCommand s_splitRegion;

    // Ctrl+R routes to the region split when regions are selected and no brush face is.
    bool SplitFaceSelected()
    {
        const selection_t &sel = KiwiSel();
        for ( size_t i = 0; i < sel.items.size(); ++i )
            if ( sel.items[i].kind == SEL_FACE && Sel_BrushLive( sel.items[i].brush ) )
                return true;
        return false;
    }
}

// Cut needs only a selected splittable brush - or, with none, selected construction lines
// (KIWI 2026-10-05: C then cuts the lines); the cutter is picked in the gesture.
bool KiwiSplit_CanCut()
{
    return CanCutBrushes() || CanCutLines();
}

bool KiwiSplit_CanSplitFace()
{
    const selection_t &sel = KiwiSel();
    int n = 0;
    // Match Begin's liveness test so palette state cannot advertise a freed face.
    for ( size_t i = 0; i < sel.items.size(); ++i )
        if ( sel.items[i].kind == SEL_FACE && Sel_BrushLive( sel.items[i].brush ) )
            ++n;
    if ( n == 0 && KiwiRegion_HasSelection() )
        return true;                    // KIWI: Ctrl+R on regions = bisecting line
    if ( n == 0 )
    {
        // KIWI (2026-10-22): brush mode - a selected splittable brush splits along its
        // clicked face.
        for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
            if ( Splittable( b ) )
                return true;
        return false;
    }
    return n == 1;
}

void KiwiSplit_RegisterCommands()
{
    Radiant_RegisterCommand( "KiwiCut",       0, 0, KIWI_CMD_CUT );
    Radiant_RegisterCommand( "KiwiSplitFace", 0, 0, KIWI_CMD_SPLIT_FACE );
}

KiwiEditorCommand *KiwiSplit_CommandForId( int commandId )
{
    if ( commandId == KIWI_CMD_CUT )
    {
        // KIWI (2026-10-05): no brush to cut but construction lines selected - cut the lines.
        // A brush in the selection keeps the brush Cut (selected lines then seed its fence).
        if ( !CanCutBrushes() && CanCutLines() )
            return &s_cutLines;
        return &s_cut;
    }
    if ( commandId == KIWI_CMD_SPLIT_FACE )
    {
        // KIWI (2026-10-24, user: "If I want to do repeated ctrl-r operations, it doesn't work
        // and actually wrongly does a 4-style brush cut ... I want to be able to bisect
        // construction faces with Ctrl-R"): a selected construction face always takes the
        // bisecting-line split, also when it lies on a brush face (face mode's own halves).
        if ( !SplitFaceSelected() && KiwiRegion_HasSelection() )
            return &s_splitRegion;
        return &s_splitFace;
    }
    return 0;
}

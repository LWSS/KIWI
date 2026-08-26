#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Cut and Face Split UI over the ported brush splitter; see kiwi_split.h for
// plane orientation, ownership, validity, and undo contracts.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"                // camera_s
#include <gfx_d3d/r_gfx.h>          // GfxColor
#include <gfx_d3d/r_material.h>     // Material
#include <gfx_d3d/r_rendercmds.h>   // MaterialTechniqueType, TECHNIQUE_UNLIT

#include "kiwi_split.h"
#include "kiwi_camera.h"                    // cut-disc world/pixel scale
#include "kiwi_command.h"
#include "kiwi_fmt.h"
#include "kiwi_construct.h"
#include "kiwi_grid.h"                      // world-anchored lattice
#include "kiwi_lines.h"
#include "kiwi_material.h"                  // split-face inheritance
#include "kiwi_numeric.h"                   // Face Split offset
#include "kiwi_pick.h"
#include "kiwi_selection.h"
#include "kiwi_snap.h"                      // geometry snap classification
#include "kiwi_units.h"                     // HUD offset display
#include "kiwi_validity.h"
#include "kiwi_vec.h"                       // shared vector helpers

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>

// Ported entry points.
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

// Same fill path as kiwi_region.cpp.
extern char        Byte4PackPixelColor( float *from, GfxColor *out );          // 0x402ac0
extern void        __cdecl R_AddRenderCmdDrawTris(
                       Material *material, MaterialTechniqueType techType, short indexCount,
                       const uint16_t *indices, short vertexCount,
                       const float ( *xyzw )[4], const float ( *normal )[3], float *color,
                       const float ( *st )[2] );                               // 0x4fd1c0

namespace
{
    // 0.22 matches region fills; Plasticity's 0.10 is unreadable at KIWI brightness.
    const float KSPLIT_PLANE_RGBA[4] = { 1.00f, 0.16f, 0.16f, 0.22f };
    const float KSPLIT_LINE_COL[3]   = { 1.00f, 0.35f, 0.30f };   // the intersection outline
    const float KSPLIT_EDGE_COL[3]   = { 1.00f, 0.75f, 0.35f };   // the cut LINE itself
    // Match the editor's existing hot-handle yellow.
    const float KSPLIT_HOT_COL[3]    = { 1.00f, 0.90f, 0.30f };

    const float KSPLIT_EPS = 1.0e-4f;

    // World units: accept coplanar snaps within weld-scale error and keep the cut
    // away from both span ends so neither half is an immediate §19 sliver.
    const float KSPLIT_FACE_EPS  = 0.1f;
    const float KSPLIT_MIN_SPAN  = 1.0f;

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
        out->d    = Dot3( n, w->p[0] );
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

    brush_t *front = 0, *back = 0;
    Brush_SplitBrushByFace( def, &clipFace, &front, &back );

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

    // C — Cut. The source click locks the plane described in kiwi_split.h.
    class KiwiCutCommand : public KiwiEditorCommand
    {
    public:
        const char *Name() const override { return "Cut"; }
        bool CanExecute() override { return KiwiSplit_CanCut(); }

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
            if ( m_stage != KCUT_PICK_LINE )
                return true;
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
                m_fenceObjs.clear();
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
                    // m_p remains single-plane state; fence geometry lives separately.
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
                // One segment uses the ordinary single-plane path.
                Copy3( &m_fence[0], m_hover.a );
                Copy3( &m_fence[3], m_hover.b );
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
            m_fenceObjs.clear();      // additive construction sources
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
            for ( size_t i = 0; i < m_fenceObjs.size(); ++i )
                if ( m_fenceObjs[i] == obj )
                    return true;
            return false;
        }

        // Rebuild flattened world segments and their shared sweep direction together.
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
        // Fence objects stay in pick order; flattened segments use six floats each.
        // FenceRebuild updates segments, sweep normal, and its source together.
        std::vector<int>   m_fenceObjs;
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
            m_haveT    = false;             // seed at centroid until mouse movement

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
            // State the convex-brush limitation before commit.
            Sys_Printf( "Split Face: move the mouse to slide the cut, Tab flips U/V, "
                        "type an exact offset, RMB / Enter splits.\n"
                        "Split Face: this splits the BRUSH along that line - a brush "
                        "face cannot be split on its own (a convex plane-brush has no "
                        "way to carry a divided face).\n" );
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

                // Global geometry snaps qualify only on this face plane. Exclude
                // SNAP_FACE because it is the unsnapped surface hit and would starve grid snap.
                if ( snap.valid && KiwiSnap_IsGeometry( snap.type )
                  && snap.type != SNAP_FACE
                  && fabsf( Dot3( n, snap.position ) - d ) <= KSPLIT_FACE_EPS )
                {
                    Copy3( snap.position, world );
                    have = true;
                }
                // Otherwise intersect the selected face plane and grid-snap it.
                // A valid SNAP_NONE signals Ctrl suppression and preserves the raw hit.
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

                    // Apply the screen-space center latch last so explicit geometry
                    // snaps win; SNAP_NONE/Ctrl and typed input suppress it.
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

        // Rebuild face basis and projected span independently of cut position.
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
            if ( !( m_hi - m_lo > KSPLIT_MIN_SPAN ) )
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
                // Display offset with total span so its low-edge reference has scale.
                char offset[48], span[48];
                _snprintf( m_hud, sizeof( m_hud ),
                           "split brush  %s  offset %s / %s%s  (Tab flips)",
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
        int   m_axis   = 0;                 // 0 = U, 1 = V
        bool  m_valid  = false;
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

    KiwiCutCommand       s_cut;
    KiwiSplitFaceCommand s_splitFace;
}

// Cut needs only a selected splittable brush; the cutter is picked in the gesture.
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
    // Match Begin's liveness test so palette state cannot advertise a freed face.
    for ( size_t i = 0; i < sel.items.size(); ++i )
        if ( sel.items[i].kind == SEL_FACE && Sel_BrushLive( sel.items[i].brush ) )
            ++n;
    return n == 1;
}

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

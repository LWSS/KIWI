#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// Loft geometry is built unlinked, validity-gated, then landed through
// KiwiExtrude_LandDef so rejected brushes never touch the map.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"                // camera_s (the eye-orient reference)
#include <gfx_d3d/r_gfx.h>          // GfxColor, Byte4PackPixelColor
#include <gfx_d3d/r_material.h>     // Material
#include <gfx_d3d/r_rendercmds.h>   // MaterialTechniqueType, TECHNIQUE_UNLIT

#include "kiwi_loft.h"
#include "kiwi_csg.h"
#include "kiwi_command.h"
#include "kiwi_extrude.h"           // KiwiExtrude_LandDef
#include "kiwi_lines.h"
#include "kiwi_material.h"
#include "kiwi_numeric.h"
#include "kiwi_pick.h"
#include "kiwi_selection.h"
#include "kiwi_units.h"
#include "kiwi_validity.h"
#include "radiant_registry.h"
#include "kiwi_vec.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <vector>

// Ported entry points.
extern int         Sys_Printf( const char *fmt, ... );                        // win_qe3.cpp
extern camera_s   *Ed_Camera();                                               // camwnd.cpp:161
extern int         g_nUpdateBits;                                             // 0x25D5A74 (mainfrm.cpp)
// The ported parameter name is misleading: callers pass the material-definition source.
extern brush_t    *Brush_Alloc( const void *planeptsSrc, eclass_t *ecls );    // brush.cpp:465  0x4751e0
extern void        Brush_Free_R( brush_t *def );                              // brush.cpp:706  0x475af0
extern void        Select_Deselect( int bAlsoFreeFaces );                     // select.cpp:1444 0x48E800

// brush.cpp / xywnd.cpp KIWI forwarders.
extern void Ed_BrushSetFaceCount( brush_t *def, int faceCount );
extern void Ed_EnsureCurrentMaterial_Kiwi();

// Patch creation entry points; the landing order mirrors kiwi_patchfillet.cpp.
extern selbrush_t  *Brush_AddToList( brush_t *def, entity_s *owner );         // brush.cpp:669  0x475980
extern void         Brush_AddToList2( selbrush_t *b );                        // brush.cpp:927  0x4765A0
extern patchMesh_t *MakeNewPatch();                                           // pmesh.cpp:136  0x437AC0
extern brush_t     *AddBrushForPatch( patchMesh_t *p, entity_s *world_ent );  // pmesh.cpp:840  0x4386A0
extern void         Patch_KiwiFinishNewLike( patchMesh_t *p, const texdef_sub_t *srcTex );
extern void         Patch_KiwiCapAlign( patchMesh_t *p );                     // pmesh.cpp

// Translucent preview fill.
extern char        Byte4PackPixelColor( float *from, GfxColor *out );          // 0x402ac0
extern void        __cdecl R_AddRenderCmdDrawTris(
                       Material *material, MaterialTechniqueType techType, short indexCount,
                       const uint16_t *indices, short vertexCount,
                       const float ( *xyzw )[4], const float ( *normal )[3], float *color,
                       const float ( *st )[2] );                               // 0x4fd1c0

// Keep file-scoped: MSVC mangles a block-scoped extern inside this namespace.
extern bool        Radiant_RegisterCommand( const char *name, byte vk, byte mods,
                                            int commandId );                   // mainfrm.cpp:1340

namespace
{
    // NumericFields copies descriptors but not labels, so this storage must persist.
    // Both values are dimensionless and undo the numeric layer's unit conversion.
    const kiwiNumField_t KLOFT_FIELDS[2] =
    { { "density", KNUM_COUNT,  false },
      { "tension", KNUM_FACTOR, false } };

    // Match the preview, invalid, and hot colours used by the other KIWI tools.
    const float KLOFT_COL_PREVIEW[3] = { 0.55f, 0.85f, 1.00f };
    const float KLOFT_COL_BAD    [3] = { 1.00f, 0.30f, 0.25f };
    const float KLOFT_COL_HOT    [3] = { 1.00f, 0.90f, 0.30f };
    const float KLOFT_COL_ENDS   [3] = { 0.35f, 1.00f, 0.55f };   // the two SOURCE rings
    const float KLOFT_FILL_OK  [4] = { 0.55f, 0.85f, 1.00f, 0.22f };
    const float KLOFT_FILL_BAD [4] = { 1.00f, 0.30f, 0.25f, 0.22f };

    const float KLOFT_EPS = 1.0e-6f;

    // Bind the per-patch span cap to the patch-format width.
    static_assert( KLOFT_CURVE_SPANS_PER_PATCH * 2 + 1 == KPATCH_MAX_WIDTH,
                   "KLOFT_CURVE_SPANS_PER_PATCH must be (KPATCH_MAX_WIDTH - 1) / 2" );
    static_assert( KLOFT_MAX_CURVE_SEGS >= KLOFT_CURVE_SPANS_PER_PATCH,
                   "the CURVE density ceiling cannot be below one patch's worth" );

    // World-space face-plane membership tolerance; matches KBOOL_ONPLANE_EPS.
    const float KLOFT_ONPLANE_EPS = 0.01f;

    // Reject arc handles farther than this chord multiple; near-parallel tangents
    // otherwise produce unbounded patches. A quarter-circle handle is about 0.71.
    const float KLOFT_HANDLE_MAX = 2.0f;

    typedef std::vector<float> ring_t;      // 3 floats per point, WORLD space

    inline int RingN( const ring_t &r ) { return (int)( r.size() / 3 ); }
    inline const float *RingP( const ring_t &r, int i ) { return &r[(size_t)i * 3]; }
    inline       float *RingP( ring_t &r, int i )       { return &r[(size_t)i * 3]; }

    void RingCentroid( const ring_t &r, float out[3] )
    {
        out[0] = out[1] = out[2] = 0.0f;
        const int n = RingN( r );
        if ( n <= 0 )
            return;
        for ( int i = 0; i < n; ++i )
        {
            const float *p = RingP( r, i );
            out[0] += p[0]; out[1] += p[1]; out[2] += p[2];
        }
        out[0] /= (float)n; out[1] /= (float)n; out[2] /= (float)n;
    }

    // Newell's method is stable for nearly planar rings and collapses on degeneracy.
    bool RingNormal( const ring_t &r, float out[3] )
    {
        out[0] = out[1] = out[2] = 0.0f;
        const int n = RingN( r );
        if ( n < 3 )
            return false;
        for ( int i = 0; i < n; ++i )
        {
            const float *a = RingP( r, i );
            const float *b = RingP( r, ( i + 1 ) % n );
            out[0] += ( a[1] - b[1] ) * ( a[2] + b[2] );
            out[1] += ( a[2] - b[2] ) * ( a[0] + b[0] );
            out[2] += ( a[0] - b[0] ) * ( a[1] + b[1] );
        }
        return Norm3( out );
    }

    // Project onto the Newell plane so adjacent brush segments share exact cap planes.
    bool PlanariseRing( ring_t *r, float outN[3], float *outD )
    {
        if ( !RingNormal( *r, outN ) )
            return false;
        float c[3];
        RingCentroid( *r, c );
        const float d = Dot3( outN, c );
        const int n = RingN( *r );
        for ( int i = 0; i < n; ++i )
        {
            float *p = RingP( *r, i );
            Mad3( p, outN, d - Dot3( outN, p ), p );
        }
        if ( outD )
            *outD = d;
        return true;
    }

    void ReverseRing( ring_t *r )
    {
        const int n = RingN( *r );
        for ( int i = 0, j = n - 1; i < j; ++i, --j )
            for ( int k = 0; k < 3; ++k )
            {
                const float t = ( *r )[(size_t)i * 3 + k];
                ( *r )[(size_t)i * 3 + k] = ( *r )[(size_t)j * 3 + k];
                ( *r )[(size_t)j * 3 + k] = t;
            }
    }

    // Split longest edges to equalize counts without moving or rounding source corners.
    void SubdivideTo( ring_t *r, int target )
    {
        while ( RingN( *r ) < target )
        {
            const int n = RingN( *r );
            int   best = 0;
            float bestLen = -1.0f;
            for ( int i = 0; i < n; ++i )
            {
                float e[3];
                Sub3( RingP( *r, ( i + 1 ) % n ), RingP( *r, i ), e );
                const float l = Len3( e );
                if ( l > bestLen ) { bestLen = l; best = i; }
            }
            const float *a = RingP( *r, best );
            const float *b = RingP( *r, ( best + 1 ) % n );
            const float mid[3] = { ( a[0]+b[0] )*0.5f, ( a[1]+b[1] )*0.5f, ( a[2]+b[2] )*0.5f };
            r->insert( r->begin() + (size_t)( best + 1 ) * 3, mid, mid + 3 );
        }
    }

    void RotateRing( ring_t *r, int k )
    {
        const int n = RingN( *r );
        if ( n <= 0 || ( k % n ) == 0 )
            return;
        ring_t out;
        out.reserve( r->size() );
        for ( int i = 0; i < n; ++i )
        {
            const float *p = RingP( *r, ( i + k ) % n );
            out.push_back( p[0] ); out.push_back( p[1] ); out.push_back( p[2] );
        }
        r->swap( out );
    }

    // Use the least-aligned world axis for a deterministic, reproducible tangent.
    bool FrameTangent( const float n[3], float u[3] )
    {
        float ax[3] = { 0.0f, 0.0f, 0.0f };
        const float an[3] = { fabsf( n[0] ), fabsf( n[1] ), fabsf( n[2] ) };
        if ( an[0] <= an[1] && an[0] <= an[2] )      ax[0] = 1.0f;
        else if ( an[1] <= an[2] )                   ax[1] = 1.0f;
        else                                          ax[2] = 1.0f;
        Cross3( ax, n, u );
        return Norm3( u );
    }

    // Rodrigues rotation about unit axis w.
    void Rodrigues( const float w[3], float cosT, float sinT,
                    const float in[3], float out[3] )
    {
        float cr[3];
        Cross3( w, in, cr );
        const float d = Dot3( w, in ) * ( 1.0f - cosT );
        for ( int k = 0; k < 3; ++k )
            out[k] = in[k] * cosT + cr[k] * sinT + w[k] * d;
    }

    // Minimal unit-normal rotation. False means parallel (identity) or antipodal;
    // the caller distinguishes them by cosOut because antipodal twist is undefined.
    bool MinRotation( const float from[3], const float to[3],
                      float axisOut[3], float *cosOut, float *sinOut )
    {
        float w[3];
        Cross3( from, to, w );
        const float s = Len3( w );
        const float c = Dot3( from, to );
        *cosOut = c;
        *sinOut = s;
        if ( s < KLOFT_EPS )
        {
            axisOut[0] = 1.0f; axisOut[1] = axisOut[2] = 0.0f;
            return false;                 // parallel (c ~ +1) or antipodal (c ~ -1)
        }
        axisOut[0] = w[0] / s; axisOut[1] = w[1] / s; axisOut[2] = w[2] / s;
        return true;
    }

    // Ring coordinates in a transported frame; out2 stores two floats per vertex.
    void RingToFrame( const ring_t &r, const float c[3], const float u[3],
                      const float v[3], std::vector<float> *out2 )
    {
        const int n = RingN( r );
        out2->assign( (size_t)n * 2, 0.0f );
        for ( int i = 0; i < n; ++i )
        {
            float d[3];
            Sub3( RingP( r, i ), c, d );
            ( *out2 )[(size_t)i * 2 + 0] = Dot3( d, u );
            ( *out2 )[(size_t)i * 2 + 1] = Dot3( d, v );
        }
    }

    // Find the least-squares cyclic correspondence in aligned 2D frames. Comparing
    // in world/axis projection would squash non-parallel profiles and induce twist.
    int BestOffset2D( const std::vector<float> &a2, const std::vector<float> &b2 )
    {
        const int n = (int)( a2.size() / 2 );
        if ( n <= 0 || (int)( b2.size() / 2 ) != n )
            return 0;

        int   bestK = 0;
        float bestC = 3.4e38f;
        for ( int k = 0; k < n; ++k )
        {
            float cost = 0.0f;
            for ( int i = 0; i < n; ++i )
            {
                const int j = ( i + k ) % n;
                const float dx = a2[(size_t)i*2+0] - b2[(size_t)j*2+0];
                const float dy = a2[(size_t)i*2+1] - b2[(size_t)j*2+1];
                cost += dx*dx + dy*dy;
                if ( cost >= bestC )
                    break;                // this k is already worse; stop paying for it
            }
            if ( cost < bestC ) { bestC = cost; bestK = k; }
        }
        return bestK;
    }

    // Face_MakePlane and map serialization consume planepts, not a trusted plane.
    // Keep points well spread and ordered so cross(p0-p1, p2-p1) is exactly +n.
    void PlanePts( const float n[3], float dist, const float anchor[3], float spread,
                   float out[3][3] )
    {
        // Cross with the least-aligned world axis to avoid a parallel tangent.
        float ax[3] = { 0.0f, 0.0f, 0.0f };
        const float an[3] = { fabsf( n[0] ), fabsf( n[1] ), fabsf( n[2] ) };
        if ( an[0] <= an[1] && an[0] <= an[2] )      ax[0] = 1.0f;
        else if ( an[1] <= an[2] )                   ax[1] = 1.0f;
        else                                          ax[2] = 1.0f;

        float t[3], u[3];
        Cross3( ax, n, t );
        if ( !Norm3( t ) ) { t[0] = 1.0f; t[1] = t[2] = 0.0f; }
        Cross3( n, t, u );                    // t × u == n by construction
        Norm3( u );

        float c[3];
        Mad3( anchor, n, dist - Dot3( n, anchor ), c );   // anchor dropped onto the plane

        Copy3( c, out[1] );
        Mad3( c, t, spread, out[0] );
        Mad3( c, u, spread, out[2] );
    }

    // Build one unlinked segment from corresponded, planar, equal-count rings.
    // The caller rebuilds and gates it before either landing or freeing it.
    brush_t *BuildSegment( const ring_t &lo, const float loN[3], float loD,
                           const ring_t &hi, const float hiN[3], float hiD,
                           const brush_t *mtlDef, int mtlFace, const char **why )
    {
        const int n = RingN( lo );
        if ( n < 3 || RingN( hi ) != n )
        {
            *why = "the two station rings do not match";
            return 0;
        }
        if ( n > KLOFT_MAX_RING )
        {
            *why = "profile over the ring cap";
            return 0;
        }

        // Orient every plane away from this centre and scale planepts to the segment.
        float mid[3] = { 0.0f, 0.0f, 0.0f };
        {
            float cl[3], ch[3];
            RingCentroid( lo, cl );
            RingCentroid( hi, ch );
            for ( int k = 0; k < 3; ++k )
                mid[k] = ( cl[k] + ch[k] ) * 0.5f;
        }
        float spread = 32.0f;
        for ( int i = 0; i < n; ++i )
        {
            float d[3];
            Sub3( RingP( lo, i ), mid, d );  const float a = Len3( d );
            Sub3( RingP( hi, i ), mid, d );  const float b = Len3( d );
            if ( a > spread ) spread = a;
            if ( b > spread ) spread = b;
        }

        Ed_EnsureCurrentMaterial_Kiwi();
        brush_t *def = Brush_Alloc( g_qeglobals.random_texture_stuff, 0 );
        if ( !def )
        {
            *why = "brush allocation failed";
            return 0;
        }
        Ed_BrushSetFaceCount( def, n + 2 );
        if ( !def->faces || def->faceCount != n + 2 )
        {
            Brush_Free_R( def );              // still unlinked, refCount 0
            *why = "face array allocation failed";
            return 0;
        }

        // Cap planes use their station planes, oriented away from the segment centre.
        {
            float nrm[3]; Copy3( loN, nrm );
            float d = loD;
            float toMid[3];
            {
                float c[3]; RingCentroid( lo, c );
                Sub3( mid, c, toMid );
            }
            if ( Dot3( nrm, toMid ) > 0.0f )
            {                                  // pointing INTO the solid — flip it
                nrm[0] = -nrm[0]; nrm[1] = -nrm[1]; nrm[2] = -nrm[2];
                d = -d;
            }
            PlanePts( nrm, d, mid, spread, def->faces[0].planepts );
        }
        {
            float nrm[3]; Copy3( hiN, nrm );
            float d = hiD;
            float toMid[3];
            {
                float c[3]; RingCentroid( hi, c );
                Sub3( mid, c, toMid );
            }
            if ( Dot3( nrm, toMid ) > 0.0f )
            {
                nrm[0] = -nrm[0]; nrm[1] = -nrm[1]; nrm[2] = -nrm[2];
                d = -d;
            }
            PlanePts( nrm, d, mid, spread, def->faces[1].planepts );
        }

        // Side quads may be non-planar. Use their Newell direction, then push the
        // plane through the outermost corner so no source vertex is clipped.
        for ( int e = 0; e < n; ++e )
        {
            const int e1 = ( e + 1 ) % n;
            ring_t quad;
            quad.reserve( 12 );
            const float *q[4] = { RingP( lo, e ), RingP( lo, e1 ),
                                  RingP( hi, e1 ), RingP( hi, e ) };
            for ( int k = 0; k < 4; ++k )
            {
                quad.push_back( q[k][0] ); quad.push_back( q[k][1] ); quad.push_back( q[k][2] );
            }

            float nrm[3];
            if ( !RingNormal( quad, nrm ) )
            {
                // Recover the limiting plane of a zero-height quad; refuse if it too collapses.
                float ea[3], axis[3];
                Sub3( q[1], q[0], ea );
                Sub3( mid, q[0], axis );
                Cross3( ea, axis, nrm );
                if ( !Norm3( nrm ) )
                {
                    Brush_Free_R( def );
                    *why = "a side face collapsed (the two profiles touch)";
                    return 0;
                }
            }
            // Outward = away from the solid's centre.
            {
                float toMid[3];
                Sub3( mid, q[0], toMid );
                if ( Dot3( nrm, toMid ) > 0.0f )
                { nrm[0] = -nrm[0]; nrm[1] = -nrm[1]; nrm[2] = -nrm[2]; }
            }
            float d = Dot3( nrm, q[0] );
            for ( int k = 1; k < 4; ++k )
            {
                const float dk = Dot3( nrm, q[k] );
                if ( dk > d ) d = dk;          // push out until all four are inside
            }
            PlanePts( nrm, d, mid, spread, def->faces[2 + e].planepts );
        }

        // Seed from the chosen source face; tool materials fall through the standard
        // brush fallback ladder instead of inventing a visible surface.
        if ( mtlDef && mtlDef->faces && mtlFace >= 0 && mtlFace < mtlDef->faceCount )
        {
            const bool direct = KiwiMtl_FaceIsInheritable( &mtlDef->faces[mtlFace] );
            for ( int f = 0; f < def->faceCount; ++f )
            {
                if ( direct )
                    KiwiMtl_SeedFaceFrom( &def->faces[f], mtlDef, mtlFace );
                else
                {
                    // The new plane is not rebuilt yet; score fallback material against
                    // the source-face normal that this surface continues.
                    KiwiMtl_SeedClipFace( &def->faces[f], mtlDef,
                                          mtlDef->faces[mtlFace].plane.normal );
                }
                // Copying can propagate dead lightmap/smoothing channels; repair them.
                KiwiMtl_EnsureFaceLayers( &def->faces[f] );   // kiwi_material.h:245
            }
        }
        return def;
    }

    // kiwi_lines cannot express alpha, so draw one translucent tris batch per sleeve.
    void FillSleeve( const ring_t &lo, const ring_t &hi, const float rgba[4] )
    {
        const int n = RingN( lo );
        if ( n < 3 || RingN( hi ) != n || n > KLOFT_MAX_RING )
            return;
        const camera_s *cam = Ed_Camera();
        if ( !cam )
            return;

        enum { KLOFT_FILL_VERTS = KLOFT_MAX_RING * 2,
               KLOFT_FILL_IDX   = KLOFT_MAX_RING * 6 };

        static float    s_xyzw  [KLOFT_FILL_VERTS][4];
        static float    s_normal[KLOFT_FILL_VERTS][3];
        static float    s_st    [KLOFT_FILL_VERTS][2];
        static float    s_color [KLOFT_FILL_VERTS];
        static uint16_t s_idx   [KLOFT_FILL_IDX];

        float rgbaCopy[4] = { rgba[0], rgba[1], rgba[2], rgba[3] };
        GfxColor packed;
        Byte4PackPixelColor( rgbaCopy, &packed );
        const float packedAsFloat = *(float *)&packed.packed;   // bit-cast, as the ported batcher does

        for ( int i = 0; i < n; ++i )
        {
            const float *a = RingP( lo, i );
            const float *b = RingP( hi, i );
            s_xyzw[i][0]     = a[0]; s_xyzw[i][1]     = a[1]; s_xyzw[i][2]     = a[2]; s_xyzw[i][3]     = 1.0f;
            s_xyzw[n+i][0]   = b[0]; s_xyzw[n+i][1]   = b[1]; s_xyzw[n+i][2]   = b[2]; s_xyzw[n+i][3]   = 1.0f;
            for ( int k = 0; k < 2; ++k )
            {
                const int v = i + k * n;
                // Fill normals are constant in world space, never view-relative.
                KiwiTris_FillNormal( s_normal[v] );
                s_st[v][0] = 0.0f;
                s_st[v][1] = 0.0f;
                s_color[v] = packedAsFloat;
            }
        }
        for ( int e = 0; e < n; ++e )
        {
            const int e1 = ( e + 1 ) % n;
            s_idx[e*6+0] = (uint16_t)e;
            s_idx[e*6+1] = (uint16_t)e1;
            s_idx[e*6+2] = (uint16_t)( n + e1 );
            s_idx[e*6+3] = (uint16_t)e;
            s_idx[e*6+4] = (uint16_t)( n + e1 );
            s_idx[e*6+5] = (uint16_t)( n + e );
        }

        static const float s_neutral[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        static const float s_white[4]   = { 1.0f, 1.0f, 1.0f, 1.0f };
        KiwiTris_OrientToEye( &s_xyzw[0][0], 4, s_idx, n * 6, cam->origin );
        R_AddCmdSetMaterialColor( s_neutral );
        R_AddRenderCmdDrawTris( g_qeglobals.d_white, TECHNIQUE_UNLIT,
                                (short)( n * 6 ), s_idx, (short)( n * 2 ),
                                s_xyzw, s_normal, s_color, s_st );
        R_AddCmdSetMaterialColor( s_white );
    }

    // Loftable faces must pass the shared brush gate and have a bounded real winding.
    bool UsableFace( const selbrush_t *b, int face )
    {
        if ( !KiwiCsg_BrushUsable( b ) )
            return false;
        const brush_t *def = b->def;
        if ( !def->faces || face < 0 || face >= def->faceCount )
            return false;
        const winding_t *w = def->faces[face].w;
        if ( !w || w->numpoints < 3 || w->numpoints > KLOFT_MAX_RING )
            return false;
        return true;
    }

    bool FaceRing( const selbrush_t *b, int face, ring_t *out )
    {
        if ( !UsableFace( b, face ) )
            return false;
        const winding_t *w = b->def->faces[face].w;
        out->clear();
        out->reserve( (size_t)w->numpoints * 3 );
        for ( int i = 0; i < w->numpoints; ++i )
        {
            out->push_back( w->p[i][0] );
            out->push_back( w->p[i][1] );
            out->push_back( w->p[i][2] );
        }
        return true;
    }

    // Curve continuity and interior visibility persist across Reset/Begin.
    int  s_contStart = KLOFT_CONT_G1;
    int  s_contEnd   = KLOFT_CONT_G1;
    // A single sheet is generated outward. Interior visibility adds a coincident,
    // row-mirrored copy and is off by default to avoid duplicate UV surfaces.
    bool s_twoSided  = false;
    bool s_prefsRead = false;

    void LoftPrefs_Read()
    {
        if ( s_prefsRead )
            return;
        s_prefsRead = true;
        s_contStart = Radiant_ProfileGetInt( KLOFT_SECTION, "ContStart", KLOFT_CONT_G1 );
        s_contEnd   = Radiant_ProfileGetInt( KLOFT_SECTION, "ContEnd",   KLOFT_CONT_G1 );
        // Do not reuse the old TwoSided key: enabled values encoded the obsolete
        // inward-winding workaround, not an intentional interior-visible choice.
        s_twoSided  = ( Radiant_ProfileGetInt( KLOFT_SECTION, "TwoSided2", 0 ) != 0 );
        if ( s_contStart < 0 || s_contStart >= KLOFT_CONT_COUNT ) s_contStart = KLOFT_CONT_G1;
        if ( s_contEnd   < 0 || s_contEnd   >= KLOFT_CONT_COUNT ) s_contEnd   = KLOFT_CONT_G1;
    }

    void LoftPrefs_Write( int contStart, int contEnd, bool twoSided )
    {
        s_prefsRead = true;
        s_contStart = contStart;
        s_contEnd   = contEnd;
        s_twoSided  = twoSided;
        Radiant_ProfileSetInt( KLOFT_SECTION, "ContStart", contStart );
        Radiant_ProfileSetInt( KLOFT_SECTION, "ContEnd",   contEnd );
        Radiant_ProfileSetInt( KLOFT_SECTION, "TwoSided2", twoSided ? 1 : 0 );
    }

    const char *ContName( int c )
    {
        return ( c == KLOFT_CONT_G0 ) ? "G0"
             : ( c == KLOFT_CONT_G2 ) ? "G2" : "G1";
    }

    class KiwiLoftCommand : public KiwiEditorCommand
    {
    public:
        const char *Name() const override { return "Loft"; }
        bool CanExecute() override { return KiwiLoft_CanExecute(); }

        // Face selection owns click callbacks; Enter/RMB remains the commit path.
        bool WantsClicks() const override { return true; }

        int NumericFields( const kiwiNumField_t **out ) const override
        { *out = KLOFT_FIELDS; return 2; }

        bool NumericFieldValue( int field, float *out ) const override
        {
            if ( !out )
                return false;
            if ( field == 0 ) { *out = (float)m_segs;  return true; }
            if ( field == 1 ) { *out = m_tension;      return true; }
            return false;
        }

        // Both fields are dimensionless, so undo the numeric layer's unit conversion.
        void NumericFieldChanged( int field, bool has, float world ) override
        {
            if ( !has )
                return;                        // cleared: keep what the tool has
            if ( field == 0 )
            {
                const int asked = (int)floorf( Units_ToDisplay( world ) + 0.5f );
                m_segs      = ClampSegs( asked );
                // Report only clamps of explicit input; mode-change reclamps are implicit.
                if ( asked > m_segs )
                    Sys_Printf( "Loft: density %i reduced to %i — %s.\n",
                                asked, m_segs,
                                UsePatches()
                                ? "that is the CURVE density ceiling — a strip is cut "
                                  "into patches of 7 spans each and one loft may not "
                                  "exceed the patch budget"
                                : "that is this tool's density ceiling" );
                m_segsTyped = true;            // …and a mode flip must not re-seed it
                Rebuild();
                UpdateHud();
                g_nUpdateBits |= 1;
                return;
            }
            if ( field == 1 )
            {
                float t = Units_ToDisplay( world );
                if ( t < KLOFT_MIN_TENSION ) t = KLOFT_MIN_TENSION;
                if ( t > KLOFT_MAX_TENSION ) t = KLOFT_MAX_TENSION;
                m_tension = t;
                Rebuild();
                UpdateHud();
                g_nUpdateBits |= 1;
                return;
            }
            KiwiEditorCommand::NumericFieldChanged( field, has, world );
        }

        // Options are available only once both faces exist. Handlers remain the
        // single source of clamping, persistence, and rebuild rules.
        int CommandOptions( const kiwiOption_t **out ) const override
        {
            if ( m_stage != KLOFT_LIVE )
                return 0;
            static const char *const s_modes[KLOFT_BRIDGE_COUNT] =
                { "Ruled", "Tangent", "Curve" };
            // Static labels outlive the call; copy the template to patch in the
            // mode-dependent density ceiling.
            static const kiwiOption_t s_optsTemplate[4] = {
                { "Bridge",  KOPT_ENUM,     s_modes, KLOFT_BRIDGE_COUNT, 0,
                  0.0f, 0.0f, -1 },
                { "Density", KOPT_INT,      0, 0, 0,
                  (float)KLOFT_MIN_SEGS, (float)KLOFT_MAX_SEGS, -1 },
                { "Tension", KOPT_NUMFIELD, 0, 0, 1,
                  KLOFT_MIN_TENSION, KLOFT_MAX_TENSION, 0 },
                { "Twist",   KOPT_INT,      0, 0, 0,
                  -(float)( KLOFT_MAX_RING - 1 ), (float)( KLOFT_MAX_RING - 1 ), -1 },
            };
            // enabledBy cannot express Bridge == CURVE, so omit patch-only settings
            // outside curve mode instead of leaving active controls that do nothing.
            static const char *const s_cont[KLOFT_CONT_COUNT] = { "G0", "G1", "G2" };
            static const kiwiOption_t s_curveTemplate[3] = {
                { "Start continuity", KOPT_ENUM,   s_cont, KLOFT_CONT_COUNT, 0,
                  0.0f, 0.0f, -1 },
                { "End continuity",   KOPT_ENUM,   s_cont, KLOFT_CONT_COUNT, 0,
                  0.0f, 0.0f, -1 },
                { "Interior visible", KOPT_TOGGLE, 0, 0, 0,
                  0.0f, 0.0f, -1 },
            };
            static kiwiOption_t s_opts[7];
            for ( int i = 0; i < 4; ++i )
                s_opts[i] = s_optsTemplate[i];
            s_opts[1].hi = (float)MaxSegs();
            if ( !UsePatches() )
            {
                *out = s_opts;
                return 4;
            }
            for ( int i = 0; i < 3; ++i )
                s_opts[4 + i] = s_curveTemplate[i];
            *out = s_opts;
            return 7;
        }

        int OptionValue( int opt ) const override
        {
            if ( opt == 0 ) return m_bridge;
            if ( opt == 1 ) return m_segs;
            if ( opt == 3 ) return m_twist;
            if ( opt == 4 ) return m_cont[0];
            if ( opt == 5 ) return m_cont[1];
            if ( opt == 6 ) return m_twoSided ? 1 : 0;
            return 0;
        }

        void OptionChanged( int opt, int value ) override
        {
            if ( opt == 0 )
            {
                // Set directly: cycling D could take two presses and choose a direction.
                if ( value >= 0 && value < KLOFT_BRIDGE_COUNT && value != m_bridge )
                    SetBridge( value );
                return;
            }
            if ( opt == 1 )
            {
                // Route through the numeric handler for clamping and the typed latch.
                NumericFieldChanged( 0, true, Units_FromDisplay( (float)value ) );
                return;
            }
            if ( opt == 3 )
            {
                // Ring count is unknown here; BuildStations reduces this modulo n.
                m_twist = value;
                Rebuild();
                UpdateHud();
                g_nUpdateBits = -1;
                return;
            }
            // Persist curve settings immediately and keep preview geometry current.
            if ( opt == 4 || opt == 5 )
            {
                if ( value < 0 || value >= KLOFT_CONT_COUNT )
                    return;
                m_cont[opt - 4] = value;
                LoftPrefs_Write( m_cont[0], m_cont[1], m_twoSided );
                Rebuild();
                UpdateHud();
                Sys_Printf( "Loft: %s continuity %s — %s.\n",
                            ( opt == 4 ) ? "start" : "end", ContName( value ),
                            ( value == KLOFT_CONT_G0 )
                            ? "the end span leaves along its chord (a crease at the face)"
                            : ( value == KLOFT_CONT_G1 )
                            ? "the end span leaves along the face normal (no crease)"
                            : "arc fit: the end span is a circular arc leaving along the "
                              "face normal (an approximation — see kiwi_loft.h)" );
                g_nUpdateBits = -1;
                return;
            }
            if ( opt == 6 )
            {
                m_twoSided = ( value != 0 );
                LoftPrefs_Write( m_cont[0], m_cont[1], m_twoSided );
                Rebuild();
                UpdateHud();
                Sys_Printf( "Loft: interior visible %s — each strip is emitted %s.%s\n",
                            m_twoSided ? "ON" : "off",
                            m_twoSided ? "TWICE, the second copy row-mirrored so the "
                                         "INSIDE of the shape is textured too (two "
                                         "coincident sheets — see kiwi_loft.h)"
                                       : "once, facing OUTWARD: the inside of the "
                                         "shape will show wireframe, not texture",
                            m_twoSided ? "  (Patches carry no collision either way: "
                                         "put caulk inside the wall if the player has "
                                         "to hit it.)" : "" );
                g_nUpdateBits = -1;
            }
        }

        void SetBridge( int mode )
        {
            m_bridge = mode;
            // Re-seed mode defaults unless explicit input latched the density.
            // Typed values still obey the selected medium's structural ceiling.
            if ( !m_segsTyped )
                m_segs = DefaultSegs();
            m_segs = ClampSegs( m_segs );
            Rebuild();
            UpdateHud();
            Sys_Printf( "Loft: %s.\n",
                        m_bridge == KLOFT_BRIDGE_CURVE
                        ? "CURVE — bezier patch strips swept along the tangent path "
                          "(render only, no collision)"
                        : m_bridge == KLOFT_BRIDGE_TANGENT
                        ? "TANGENT (G1) — the bridge leaves each face along its own "
                          "normal"
                        : "RULED (G0) — a straight bridge" );
            g_nUpdateBits = -1;
        }

        const char *HudStatus() const override { return m_hud[0] ? m_hud : 0; }
        bool HudInvalid() const override { return m_stage == KLOFT_LIVE && !m_ok; }

        // Face B may be any brush face, including another face on brush A.
        unsigned PickFlags() const override { return PICKF_NONE; }

        int HudPrompts( const kiwiPrompt_t **out ) const override
        {
            static const kiwiPrompt_t s_pick[] = {
                { "LMB", "Pick the second face" },
                { "Esc", "Cancel" },
            };
            static const kiwiPrompt_t s_live[] = {
                { "D",    "Ruled / Tangent / Curve" },
                { "[ ]",  "Twist the correspondence" },
                { "LMB",  "Re-pick the second face" },
                { "Tab",  "Density / tension" },
                { "Esc",  "Pick another face" },
            };
            if ( m_stage == KLOFT_PICK_B )
            {
                *out = s_pick;
                return (int)( sizeof( s_pick ) / sizeof( s_pick[0] ) );
            }
            *out = s_live;
            return (int)( sizeof( s_live ) / sizeof( s_live[0] ) );
        }

        bool Begin() override
        {
            Reset();

            // The active face leads, followed by selected faces in selection order.
            std::vector<sel_item_t> faces;
            {
                const selection_t &sel = KiwiSel();
                if ( sel.active.kind == SEL_FACE && Sel_BrushLive( sel.active.brush )
                  && UsableFace( sel.active.brush, sel.active.faceIndex ) )
                    faces.push_back( sel.active );
                for ( size_t i = 0; i < sel.items.size(); ++i )
                {
                    const sel_item_t &it = sel.items[i];
                    if ( it.kind != SEL_FACE || !Sel_BrushLive( it.brush )
                      || !UsableFace( it.brush, it.faceIndex ) )
                        continue;
                    if ( !faces.empty() && faces[0].brush == it.brush
                      && faces[0].faceIndex == it.faceIndex )
                        continue;
                    faces.push_back( it );
                    if ( faces.size() >= 2 )
                        break;
                }
            }

            if ( faces.empty() )
            {
                Sys_Printf( "Loft: select ONE brush face and press L to pick the "
                            "other, or select TWO faces and press L to bridge them "
                            "straight away (mode 3 selects faces; patches and "
                            "fixed-size entities have no loftable face).\n" );
                return false;
            }

            m_a     = faces[0].brush;
            m_aFace = faces[0].faceIndex;

            if ( faces.size() >= 2 )
            {
                m_b     = faces[1].brush;
                m_bFace = faces[1].faceIndex;
                m_stage = KLOFT_LIVE;
                m_segs  = DefaultSegs();
                Rebuild();
                UpdateHud();
                Sys_Printf( "Loft: bridging the two selected faces — D cycles "
                            "RULED / TANGENT / CURVE, Tab picks density or tension, "
                            "RMB / Enter applies.\n" );
                return true;
            }

            m_stage = KLOFT_PICK_B;
            UpdateHud();
            Sys_Printf( "Loft: click the OTHER face — any face, on any solid "
                        "(Esc cancels).\n" );
            return true;
        }

        // Recast with a face-only mask; the framework pick follows the current
        // selection mode and may otherwise resolve a vertex, edge, or whole brush.
        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick; (void)snap;
            if ( m_stage == KLOFT_LIVE )
                return;                        // the two faces are chosen; nothing tracks

            selbrush_t *wasB    = m_hover;
            const int   wasFace = m_hoverFace;
            m_hover     = 0;
            m_hoverFace = -1;

            int   x, y;
            ray_t ray;
            if ( KiwiCmd_LastCursor( &x, &y ) && Pick_RayFromImagePos( x, y, &ray ) )
            {
                const pick_result_t hit = Pick( ray, SEL_MASK_FACE, PICKF_NONE );
                if ( hit.valid && hit.item.kind == SEL_FACE && Sel_BrushLive( hit.item.brush )
                  && UsableFace( hit.item.brush, hit.item.faceIndex )
                  && !( hit.item.brush == m_a && hit.item.faceIndex == m_aFace ) )
                {
                    m_hover     = hit.item.brush;
                    m_hoverFace = hit.item.faceIndex;
                }
            }
            if ( m_hover != wasB || m_hoverFace != wasFace )
                g_nUpdateBits |= 1;
            UpdateHud();
        }

        bool Click() override
        {
            if ( !m_hover )
            {
                Sys_Printf( "Loft: that is not a loftable face — aim at another one, "
                            "or press Esc.\n" );
                return true;
            }
            m_b     = m_hover;
            m_bFace = m_hoverFace;
            if ( m_stage != KLOFT_LIVE )
            {
                m_stage = KLOFT_LIVE;
                m_segs  = DefaultSegs();
            }
            Rebuild();
            UpdateHud();
            g_nUpdateBits = -1;
            return true;                       // never committed by a click
        }

        bool KeyDown( int vk, unsigned mods ) override
        {
            (void)mods;
            if ( vk == 0x44 && m_stage == KLOFT_LIVE )        // 'D'
            {
                SetBridge( ( m_bridge + 1 ) % KLOFT_BRIDGE_COUNT );
                return true;
            }
            if ( ( vk == 0xDB || vk == 0xDD ) && m_stage == KLOFT_LIVE )
            {
                m_twist += ( vk == 0xDD ) ? 1 : -1;
                Rebuild();
                UpdateHud();
                Sys_Printf( "Loft: twist %+i — the ring correspondence stepped by "
                            "whole vertices%s.\n", m_twist,
                            m_ok ? "" : " (still refused — keep stepping, or press D)" );
                g_nUpdateBits = -1;
                return true;
            }
            if ( vk == 0x1B && m_stage == KLOFT_LIVE )        // VK_ESCAPE
            {
                // Keep face A and walk back to the face-B picker.
                m_stage = KLOFT_PICK_B;
                m_b     = 0;
                m_bFace = -1;
                m_stations.clear();
                KiwiCmd_Resume();
                UpdateHud();
                Sys_Printf( "Loft: second face dropped — click another (Esc again "
                            "cancels).\n" );
                g_nUpdateBits = -1;
                return true;
            }
            if ( vk == 0x0D && m_stage != KLOFT_LIVE )        // VK_RETURN
            {
                Sys_Printf( "Loft: click the second face first.\n" );
                return true;
            }
            return false;
        }

        // Preview cost scales with both station and ring counts.
        int LineBudget() const override
        {
            int n = 96;                        // the snap marker's headroom
            if ( !m_stations.empty() )
            {
                const int ring = RingN( m_stations[0] );
                n += (int)m_stations.size() * ring;        // every station ring
                // Curve previews subdivide each quadratic span; brush rails do not.
                const int perSpan = UsePatches() ? KLOFT_CURVE_PREVIEW : 1;
                n += ( (int)m_stations.size() - 1 ) * ring * perSpan;
            }
            return n;
        }

        void DrawWorld() override
        {
            // Always show source rings so the latched and hovered faces remain clear.
            ring_t ra, rb;
            const bool haveA = Sel_BrushLive( m_a ) && FaceRing( m_a, m_aFace, &ra );
            if ( haveA )
            {
                KiwiLines_Color( KLOFT_COL_ENDS[0], KLOFT_COL_ENDS[1], KLOFT_COL_ENDS[2] );
                OutlineRing( ra );
            }

            if ( m_stage != KLOFT_LIVE )
            {
                if ( m_hover && Sel_BrushLive( m_hover )
                  && FaceRing( m_hover, m_hoverFace, &rb ) )
                {
                    KiwiLines_Color( KLOFT_COL_HOT[0], KLOFT_COL_HOT[1], KLOFT_COL_HOT[2] );
                    OutlineRing( rb );
                }
                return;
            }

            if ( m_stations.size() < 2 )
                return;

            const float *fill = m_ok ? KLOFT_FILL_OK : KLOFT_FILL_BAD;
            const float *line = m_ok ? KLOFT_COL_PREVIEW : KLOFT_COL_BAD;

            for ( size_t s = 0; s + 1 < m_stations.size(); ++s )
                FillSleeve( m_stations[s], m_stations[s + 1], fill );

            KiwiLines_Color( line[0], line[1], line[2] );
            for ( size_t s = 0; s < m_stations.size(); ++s )
                if ( !OutlineRing( m_stations[s] ) )
                    return;
            // Draw patch rails through the same quadratic handles used at commit;
            // straight chords would hide both curvature and correspondence errors.
            const int n = RingN( m_stations[0] );
            const int segs = (int)m_stations.size() - 1;
            if ( UsePatches() )
            {
                for ( int i = 0; i < n; ++i )
                    for ( int s = 0; s < segs; ++s )
                    {
                        const float *p0 = RingP( m_stations[(size_t)s], i );
                        const float *p1 = RingP( m_stations[(size_t)s + 1], i );
                        float t0[3], t1[3], h[3];
                        SpanTangent( s,     i, t0 );
                        SpanTangent( s + 1, i, t1 );
                        ArcHandle( p0, t0, p1, t1, SpanArcFrom( s, segs ), h );
                        float prev[3];
                        Copy3( p0, prev );
                        for ( int k = 1; k <= KLOFT_CURVE_PREVIEW; ++k )
                        {
                            const float u  = (float)k / (float)KLOFT_CURVE_PREVIEW;
                            const float iu = 1.0f - u;
                            float pt[3];
                            for ( int c = 0; c < 3; ++c )
                                pt[c] = iu * iu * p0[c] + 2.0f * iu * u * h[c] + u * u * p1[c];
                            if ( !KiwiLines_Add( prev, pt ) )
                                return;
                            Copy3( pt, prev );
                        }
                    }
                return;
            }
            for ( int i = 0; i < n; ++i )
                for ( size_t s = 0; s + 1 < m_stations.size(); ++s )
                    if ( !KiwiLines_Add( RingP( m_stations[s], i ),
                                         RingP( m_stations[s + 1], i ) ) )
                        return;
        }

        void Commit() override
        {
            if ( m_stage != KLOFT_LIVE )
            {
                Sys_Printf( "Loft: no second face was picked — nothing was done.\n" );
                Reset();
                return;
            }
            Apply();
            Reset();
            g_nUpdateBits = -1;
        }

        void Cancel() override
        {
            // Preview defs are never landed, so cancellation has nothing to restore.
            Reset();
            g_nUpdateBits |= 1;
        }

    private:
        enum stage_t { KLOFT_PICK_B = 0, KLOFT_LIVE };

        // Path shape and output medium are independent questions.
        bool UseTangent() const { return m_bridge != KLOFT_BRIDGE_RULED; }
        bool UsePatches() const { return m_bridge == KLOFT_BRIDGE_CURVE; }

        int DefaultSegs() const
        {
            if ( m_bridge == KLOFT_BRIDGE_CURVE )   return KLOFT_DEF_SEGS_CURVE;
            if ( m_bridge == KLOFT_BRIDGE_TANGENT ) return KLOFT_DEF_SEGS_TANGENT;
            return KLOFT_DEF_SEGS_RULED;
        }

        // Patch and brush modes have separate density ceilings.
        int MaxSegs() const
        { return UsePatches() ? KLOFT_MAX_CURVE_SEGS : KLOFT_MAX_SEGS; }

        int ClampSegs( int s ) const
        {
            const int hi = MaxSegs();
            if ( s < KLOFT_MIN_SEGS ) return KLOFT_MIN_SEGS;
            if ( s > hi )             return hi;
            return s;
        }

        const char *BridgeName() const
        {
            if ( m_bridge == KLOFT_BRIDGE_CURVE )   return "CURVE (patches)";
            if ( m_bridge == KLOFT_BRIDGE_TANGENT ) return "TANGENT (G1)";
            return "RULED (G0)";
        }

        // Patch output has no brush-validity gate; refuse only structural failures.
        bool CurveIsBuildable( const char **why ) const
        {
            if ( m_stations.size() < 2 )
            { *why = "no stations"; return false; }
            const int n = RingN( m_stations[0] );
            if ( n < 3 )
            { *why = "the profile has fewer than three corners"; return false; }
            const int segs = (int)m_stations.size() - 1;
            if ( n > KLOFT_MAX_RING )
            { *why = "the profiles have more corners than the ring cap"; return false; }
            // Chunks fit the grid by construction; gate the total patch product,
            // including the optional row-mirrored copies.
            const int chunks = ( segs + KLOFT_CURVE_SPANS_PER_PATCH - 1 )
                             / KLOFT_CURVE_SPANS_PER_PATCH;
            const int total  = chunks * n * ( m_twoSided ? 2 : 1 );
            if ( total > KLOFT_MAX_CURVE_PATCHES )
            { *why = "too many patches for one loft — lower the density, use a simpler "
                     "profile, or turn Interior visible off"; return false; }
            return true;
        }

        void Reset()
        {
            m_a = m_b = m_hover = 0;
            m_aFace = m_bFace = m_hoverFace = -1;
            m_stage     = KLOFT_PICK_B;
            m_bridge    = KLOFT_BRIDGE_RULED;
            m_segs      = KLOFT_DEF_SEGS_RULED;
            m_segsTyped = false;
            m_tension   = KLOFT_DEF_TENSION;
            m_twist     = 0;
            // Curve settings are preferences, not per-gesture defaults.
            LoftPrefs_Read();
            m_cont[0]    = s_contStart;
            m_cont[1]    = s_contEnd;
            m_twoSided   = s_twoSided;
            m_haveEndTan = false;
            m_outFlip    = false;
            m_ok        = false;
            m_fellBack  = false;
            m_why       = "no loft yet";
            m_stations.clear();
            m_edgeFace.clear();
            // Keep all per-station arrays in the same reset lifecycle.
            m_planeN.clear();
            m_planeD.clear();
            m_hud[0] = '\0';
        }

        bool OutlineRing( const ring_t &r )
        {
            const int n = RingN( r );
            for ( int i = 0; i < n; ++i )
                if ( !KiwiLines_Add( RingP( r, i ), RingP( r, ( i + 1 ) % n ) ) )
                    return false;
            return true;
        }

        // Construct stations separately so the dry-build fallback can retry without recursion.
        bool BuildStations()
        {
            m_stations.clear();
            m_edgeFace.clear();
            m_ok  = false;
            m_why = "unknown";

            if ( m_stage != KLOFT_LIVE || !Sel_BrushLive( m_a ) || !Sel_BrushLive( m_b ) )
            { m_why = "a source face is gone"; return false; }
            if ( m_a == m_b && m_aFace == m_bFace )
            { m_why = "the two faces are the same face"; return false; }

            ring_t A, B;
            if ( !FaceRing( m_a, m_aFace, &A ) || !FaceRing( m_b, m_bFace, &B ) )
            { m_why = "a source face is no longer loftable"; return false; }

            // 2. SENSE — both rings CCW about the loft axis.
            float cA[3], cB[3], axis[3];
            RingCentroid( A, cA );
            RingCentroid( B, cB );
            Sub3( cB, cA, axis );
            const float span = Len3( axis );
            if ( span < KLOFT_MIN_SPAN )
            { m_why = "the two faces are on top of each other"; return false; }
            Norm3( axis );

            float nA[3], nB[3];
            if ( !RingNormal( A, nA ) || !RingNormal( B, nB ) )
            { m_why = "a source winding is degenerate"; return false; }
            if ( Dot3( nA, axis ) < 0.0f ) { ReverseRing( &A ); RingNormal( A, nA ); }
            if ( Dot3( nB, axis ) < 0.0f ) { ReverseRing( &B ); RingNormal( B, nB ); }

            // G1 tangents come from source-face planes; winding normals may be rewound.
            float outA[3], outB[3];
            Copy3( m_a->def->faces[m_aFace].plane.normal, outA );
            Copy3( m_b->def->faces[m_bFace].plane.normal, outB );
            if ( Dot3( outA, axis ) < 0.0f ) { outA[0]=-outA[0]; outA[1]=-outA[1]; outA[2]=-outA[2]; }
            if ( Dot3( outB, axis ) > 0.0f ) { outB[0]=-outB[0]; outB[1]=-outB[1]; outB[2]=-outB[2]; }

            // Store both end tangents forward along A->B. Falling back to an end
            // chord would place the quadratic handle on the chord and flatten it.
            Copy3( outA, m_endTan[0] );
            for ( int kk = 0; kk < 3; ++kk )
                m_endTan[1][kk] = -outB[kk];
            m_haveEndTan = true;

            // Patch normals are cross(dCol, dRow). Here columns follow path A and
            // rows follow ring edge E, while outward is E x Newell N; therefore the
            // unflipped A x E faces inward iff dot(A,N) > 0. Flip rows exactly then.
            m_outFlip = ( Dot3( axis, nA ) > 0.0f );

            // 3. COUNTS.
            int n = RingN( A ) > RingN( B ) ? RingN( A ) : RingN( B );
            if ( n > KLOFT_MAX_RING )
            { m_why = "the profiles have more corners than the ring cap"; return false; }
            SubdivideTo( &A, n );
            SubdivideTo( &B, n );

            // Transport A's frame by the minimal nA->nB rotation so correspondence
            // compares unsquashed profiles without introducing twist.
            float uA[3], vA[3], uB[3], vB[3];
            if ( !FrameTangent( nA, uA ) )
            { m_why = "a source winding is degenerate"; return false; }
            Cross3( nA, uA, vA );
            Norm3( vA );

            float rotAxis[3], rotCos = 1.0f, rotSin = 0.0f;
            const bool rotates = MinRotation( nA, nB, rotAxis, &rotCos, &rotSin );
            if ( !rotates && rotCos < 0.0f )
            {
                // Antipodal normals do not define a unique minimal rotation or twist.
                m_why = "the two faces are back to back — no bridge direction is defined";
                return false;
            }
            if ( rotates )
            {
                Rodrigues( rotAxis, rotCos, rotSin, uA, uB );
                Norm3( uB );
            }
            else
            {
                Copy3( uA, uB );            // parallel: identity
            }
            Cross3( nB, uB, vB );
            Norm3( vB );

            // 4. CORRESPONDENCE — measured in the frames, then the manual TWIST.
            std::vector<float> a2, b2;
            RingToFrame( A, cA, uA, vA, &a2 );
            RingToFrame( B, cB, uB, vB, &b2 );
            int k = BestOffset2D( a2, b2 );
            if ( m_twist )
            {
                // Normalize manual whole-vertex steps for RotateRing's forward walk.
                k = ( ( k + m_twist ) % n + n ) % n;
            }
            RotateRing( &B, k );
            RingToFrame( B, cB, uB, vB, &b2 );      // …the coordinates follow it

            // 5. STATIONS + 6. PLANARISE.
            const int segs = ClampSegs( m_segs );
            const float mag = m_tension * span;

            m_stations.reserve( (size_t)segs + 1 );
            m_planeN.assign( (size_t)( segs + 1 ) * 3, 0.0f );
            m_planeD.assign( (size_t)( segs + 1 ), 0.0f );

            const bool tangent = UseTangent();

            for ( int s = 0; s <= segs; ++s )
            {
                const float t = (float)s / (float)segs;
                ring_t r;
                r.resize( (size_t)n * 3 );

                // RULED interpolates centroids; TANGENT/CURVE follows a Hermite path.
                // The transported frame carries the section without pinching it.
                float c[3];
                if ( !tangent )
                {
                    for ( int kk = 0; kk < 3; ++kk )
                        c[kk] = cA[kk] + ( cB[kk] - cA[kk] ) * t;
                }
                else
                {
                    // Cubic Hermite basis:
                    //   h00 = 2t³-3t²+1   h10 = t³-2t²+t
                    //   h01 = -2t³+3t²    h11 = t³-t²
                    const float t2 = t * t, t3 = t2 * t;
                    const float h00 =  2.0f*t3 - 3.0f*t2 + 1.0f;
                    const float h10 =        t3 - 2.0f*t2 + t;
                    const float h01 = -2.0f*t3 + 3.0f*t2;
                    const float h11 =        t3 -      t2;
                    for ( int kk = 0; kk < 3; ++kk )
                        c[kk] = h00 * cA[kk] + h01 * cB[kk]
                              + h10 * ( outA[kk] * mag ) + h11 * ( -outB[kk] * mag );
                }

                // Fractional minimal rotation keeps the end frames exact at t=0 and t=1.
                float un[3], uu[3], uv[3];
                if ( rotates )
                {
                    const float ang = acosf( rotCos < -1.0f ? -1.0f
                                           : rotCos >  1.0f ?  1.0f : rotCos ) * t;
                    const float ct = cosf( ang ), st = sinf( ang );
                    Rodrigues( rotAxis, ct, st, nA, un );
                    Rodrigues( rotAxis, ct, st, uA, uu );
                    Norm3( un );
                    Norm3( uu );
                }
                else
                {
                    Copy3( nA, un );
                    Copy3( uA, uu );
                }
                Cross3( un, uu, uv );
                Norm3( uv );

                // Blend in 2D, then map through the frame; congruent sections keep
                // their thickness and every station remains planar by construction.
                for ( int i = 0; i < n; ++i )
                {
                    const float x = a2[(size_t)i*2+0] + ( b2[(size_t)i*2+0] - a2[(size_t)i*2+0] ) * t;
                    const float y = a2[(size_t)i*2+1] + ( b2[(size_t)i*2+1] - a2[(size_t)i*2+1] ) * t;
                    float *o = RingP( r, i );
                    for ( int kk = 0; kk < 3; ++kk )
                        o[kk] = c[kk] + uu[kk] * x + uv[kk] * y;
                }

                float pn[3], pd = 0.0f;
                if ( s == 0 || s == segs )
                {
                    // Preserve source end planes exactly instead of refitting them.
                    if ( !RingNormal( r, pn ) )
                    { m_why = "a source winding is degenerate"; m_stations.clear(); return false; }
                    float rc[3];
                    RingCentroid( r, rc );
                    pd = Dot3( pn, rc );
                }
                else if ( !PlanariseRing( &r, pn, &pd ) )
                { m_why = "a station ring collapsed — lower the density"; m_stations.clear(); return false; }

                m_planeN[(size_t)s*3+0] = pn[0];
                m_planeN[(size_t)s*3+1] = pn[1];
                m_planeN[(size_t)s*3+2] = pn[2];
                m_planeD[(size_t)s]     = pd;
                m_stations.push_back( r );
            }

            // Resolve each profile edge's adjacent source face while ring A is in scope.
            ResolveEdgeFaces( A );

            m_ok  = true;
            m_why = "";
            return true;
        }

        // A candidate face continues edge i when its plane contains both endpoints.
        void ResolveEdgeFaces( const ring_t &A )
        {
            const int n = RingN( A );
            m_edgeFace.assign( (size_t)n, -1 );
            if ( !Sel_BrushLive( m_a ) || !m_a->def || !m_a->def->faces )
                return;
            const brush_t *def = m_a->def;
            for ( int i = 0; i < n; ++i )
            {
                const float *p0 = RingP( A, i );
                const float *p1 = RingP( A, ( i + 1 ) % n );
                for ( int f = 0; f < def->faceCount; ++f )
                {
                    if ( f == m_aFace )
                        continue;                 // the source face itself, by definition
                    const plane_t &pl = def->faces[f].plane;
                    const float d0 = Dot3( pl.normal, p0 ) - pl.dist;
                    const float d1 = Dot3( pl.normal, p1 ) - pl.dist;
                    if ( fabsf( d0 ) < KLOFT_ONPLANE_EPS && fabsf( d1 ) < KLOFT_ONPLANE_EPS )
                    {
                        m_edgeFace[(size_t)i] = f;
                        break;
                    }
                }
            }
        }

        // Dry-build and free every brush segment so the preview shows the commit's
        // validity and TANGENT->RULED fallback. Patches use the structural gate above.
        void Rebuild()
        {
            m_fellBack = false;

            if ( !BuildStations() )
                return;                          // m_ok is already false, m_why set

            if ( m_bridge == KLOFT_BRIDGE_CURVE )
            {
                const char *why = 0;
                if ( !CurveIsBuildable( &why ) )
                {
                    m_ok  = false;
                    m_why = why ? why : "the curve cannot be swept";
                }
                return;
            }

            const char *why = "unknown";
            std::vector<brush_t *> defs;
            if ( BuildAll( &defs, &why ) )
            {
                FreeAll( &defs );
                return;                          // m_ok stayed true
            }

            if ( m_bridge == KLOFT_BRIDGE_TANGENT )
            {
                // Temporarily switch mode because BuildStations reads UseTangent().
                m_bridge = KLOFT_BRIDGE_RULED;
                const bool built = BuildStations();
                const char *why2 = "unknown";
                if ( built && BuildAll( &defs, &why2 ) )
                {
                    FreeAll( &defs );
                    m_bridge   = KLOFT_BRIDGE_TANGENT;   // the USER's mode is unchanged
                    m_fellBack = true;                   // …but this is what will be built
                    m_ok       = true;
                    m_why      = "";
                    return;
                }
                m_bridge = KLOFT_BRIDGE_TANGENT;
                BuildStations();                 // put the tangent stations back to draw
                m_ok  = false;
                m_why = why2 ? why2 : why;
                return;
            }

            m_ok  = false;
            m_why = why ? why : "invalid geometry";
        }

        // Build and gate every unlinked segment; failure frees the whole set.
        bool BuildAll( std::vector<brush_t *> *out, const char **why )
        {
            out->clear();
            if ( m_stations.size() < 2 )
            { *why = m_why[0] ? m_why : "no stations"; return false; }

            // Nearest source end supplies material; the midpoint belongs to face A.
            const brush_t *defA = m_a ? m_a->def : 0;
            const brush_t *defB = m_b ? m_b->def : 0;
            const int      segs = (int)m_stations.size() - 1;

            for ( int s = 0; s < segs; ++s )
            {
                const bool nearA = ( s * 2 < segs );
                const brush_t *md = nearA ? defA : defB;
                const int      mf = nearA ? m_aFace : m_bFace;

                brush_t *def = BuildSegment( m_stations[s],     &m_planeN[(size_t)s*3],       m_planeD[s],
                                             m_stations[s + 1], &m_planeN[(size_t)( s + 1 )*3], m_planeD[s + 1],
                                             md, mf, why );
                if ( !def )
                {
                    FreeAll( out );
                    return false;
                }
                KiwiValid_Rebuild( def );
                if ( !KiwiValid_CheckBrush( def, why ) )
                {
                    Brush_Free_R( def );        // never linked: refCount 0, no owner chain
                    FreeAll( out );
                    return false;
                }
                out->push_back( def );
            }
            return !out->empty();
        }

        static void FreeAll( std::vector<brush_t *> *defs )
        {
            for ( size_t i = 0; i < defs->size(); ++i )
                Brush_Free_R( ( *defs )[i] );   // all still unlinked
            defs->clear();
        }

        void Apply()
        {
            // Rebuild already chose and dry-gated these stations; commit must not
            // independently choose different geometry.
            if ( !m_ok )
            {
                Sys_Printf( "Loft: refused — %s.  Nothing was created.  (The preview "
                            "has been RED for this reason; try a lower density, the "
                            "Twist stepper, or faces that face each other more "
                            "squarely.)\n", m_why && m_why[0] ? m_why : "invalid geometry" );
                return;
            }

            if ( UsePatches() )
            {
                ApplyCurve();
                return;
            }

            const char *why = "unknown";
            std::vector<brush_t *> defs;

            if ( !BuildAll( &defs, &why ) )
            {
                // Defensive: this indicates preview/commit divergence.
                Sys_Printf( "Loft: refused at commit — %s.  Nothing was created.\n",
                            why ? why : "invalid geometry" );
                return;
            }

            if ( m_fellBack )
                Sys_Printf( "Loft: the TANGENT bridge was rejected, so the RULED "
                            "bridge shown in the preview was built — raise the "
                            "density and re-run if you wanted the curve.\n" );

            // Open undo over an empty selection, then land the whole bridge selected.
            Select_Deselect( 1 );
            KiwiCmd_UndoBegin( "loft" );
            for ( size_t i = 0; i < defs.size(); ++i )
                KiwiExtrude_LandDef( defs[i] );      // lands SELECTED

            Sel_Clear( KiwiSel() );
            Sel_RebuildFromLegacy();

            Sys_Printf( "Loft: %s bridge — %i solid(s) over a %i-gon.\n",
                        m_fellBack ? "RULED (tangent fell back)" : BridgeName(),
                        (int)defs.size(),
                        m_stations.empty() ? 0 : RingN( m_stations[0] ) );
        }

        // Interior tangents use neighbouring stations; end tangents use source-face
        // normals unless G0 requests a chord. ArcHandle intersects the tangent lines
        // and falls back to the chord midpoint for parallel or backward solutions.
        void SpanTangent( int s, int i, float out[3] ) const
        {
            const int last = (int)m_stations.size() - 1;

            // G1/G2 use the stored face-normal direction. G0 deliberately falls
            // through to the one-sided chord direction to create a crease.
            if ( m_haveEndTan && ( s == 0 || s == last ) )
            {
                const int end = ( s == 0 ) ? 0 : 1;
                if ( m_cont[end] != KLOFT_CONT_G0 )
                {
                    Copy3( m_endTan[end], out );
                    if ( Norm3( out ) )
                        return;
                }
            }

            const int lo   = ( s > 0    ) ? s - 1 : s;
            const int hi   = ( s < last ) ? s + 1 : s;
            Sub3( RingP( m_stations[(size_t)hi], i ),
                  RingP( m_stations[(size_t)lo], i ), out );
            if ( !Norm3( out ) )
            { out[0] = 1.0f; out[1] = out[2] = 0.0f; }
        }

        // Circular-arc quadratic handle from one end:
        // P + tangent * chord/(2*cos(tangent,chord)). Refuse degenerate directions.
        bool ArcFitHandle( const float p0[3], const float t0[3],
                           const float p1[3], const float t1[3],
                           bool from1, float out[3] ) const
        {
            float d[3];
            Sub3( p1, p0, d );
            const float chord = Len3( d );
            if ( chord <= KLOFT_EPS )
                return false;
            float dir[3];
            Copy3( d, dir );
            if ( !Norm3( dir ) )
                return false;

            const float *t = from1 ? t1 : t0;
            const float  c = Dot3( t, dir );          // both tangents point FORWARD
            if ( c <= KLOFT_ARC_MIN_COS )
                return false;
            const float dist = chord / ( 2.0f * c );

            if ( from1 ) Mad3( p1, t, -dist, out );
            else         Mad3( p0, t,  dist, out );

            float off[3];
            for ( int k = 0; k < 3; ++k )
                off[k] = out[k] - ( p0[k] + p1[k] ) * 0.5f;
            return ( Len3( off ) <= KLOFT_HANDLE_MAX * chord );
        }

        // arcFrom selects start/end arc fit; a rejected fit falls back to G1 handling.
        void ArcHandle( const float p0[3], const float t0[3],
                        const float p1[3], const float t1[3], int arcFrom,
                        float out[3] ) const
        {
            if ( arcFrom == 1 || arcFrom == 2 )
            {
                float fit[3];
                if ( ArcFitHandle( p0, t0, p1, t1, arcFrom == 2, fit ) )
                {
                    Copy3( fit, out );
                    return;
                }
            }
            float d[3];
            Sub3( p1, p0, d );
            const float chord = Len3( d );
            const float a00 = Dot3( t0, t0 );
            const float a01 = Dot3( t0, t1 );
            const float a11 = Dot3( t1, t1 );
            const float det = a00 * a11 - a01 * a01;

            bool ok = false;
            float h[3] = { 0.0f, 0.0f, 0.0f };
            if ( fabsf( det ) > KLOFT_EPS && chord > KLOFT_EPS )
            {
                const float r0 = Dot3( t0, d );
                const float r1 = Dot3( t1, d );
                const float a  = (  a11 * r0 - a01 * r1 ) / det;
                const float b  = ( -a01 * r0 + a00 * r1 ) / det;
                if ( a > 0.0f && b > 0.0f )
                {
                    float q0[3], q1[3];
                    Mad3( p0, t0, a, q0 );
                    Mad3( p1, t1, -b, q1 );
                    for ( int k = 0; k < 3; ++k )
                        h[k] = ( q0[k] + q1[k] ) * 0.5f;
                    float off[3];
                    for ( int k = 0; k < 3; ++k )
                        off[k] = h[k] - ( p0[k] + p1[k] ) * 0.5f;
                    ok = ( Len3( off ) <= KLOFT_HANDLE_MAX * chord );
                }
            }
            if ( !ok )
                for ( int k = 0; k < 3; ++k )
                    h[k] = ( p0[k] + p1[k] ) * 0.5f;
            Copy3( h, out );
        }

        // Only end spans can arc-fit to source faces. If one span has G2 at both
        // ends, start wins because one quadratic cannot represent two arc fits.
        int SpanArcFrom( int s, int segs ) const
        {
            if ( s == 0 && m_cont[0] == KLOFT_CONT_G2 )
                return 1;
            if ( s == segs - 1 && m_cont[1] == KLOFT_CONT_G2 )
                return 2;
            return 0;
        }

        // Even columns are stations; odd columns are span handles. Column indices
        // are chunk-relative, but station/tangent lookups stay global to keep joins G1.
        // flip reverses rows and therefore cross(dCol,dRow), changing patch facing.
        void WriteCurveStrip( patchMesh_t *p, int e, int n, int segs,
                              int s0, int spans, bool flip ) const
        {
            for ( int s = s0; s <= s0 + spans; ++s )
            {
                const int     col = ( s - s0 ) * 2;
                const ring_t &st  = m_stations[(size_t)s];
                const float  *r0 = RingP( st, e );
                const float  *r1 = RingP( st, ( e + 1 ) % n );
                // EVEN column: the station itself.  Rows are the edge's two
                // rails and their exact midpoint — three evenly spaced collinear
                // controls describe a straight segment exactly.
                for ( int row = 0; row < KLOFT_CURVE_ROWS; ++row )
                {
                    const int   dst = flip ? ( KLOFT_CURVE_ROWS - 1 - row ) : row;
                    const float f   = (float)row / (float)( KLOFT_CURVE_ROWS - 1 );
                    for ( int k = 0; k < 3; ++k )
                        p->ctrl[col][dst].xyz[k] = r0[k] + ( r1[k] - r0[k] ) * f;
                }

                if ( s == s0 + spans )
                    continue;                  // this chunk's last column

                // ODD column: the arc handle for span s -> s+1, computed
                // independently on each rail so the strip bends with the path.
                const ring_t &nx = m_stations[(size_t)s + 1];
                const int arcFrom = SpanArcFrom( s, segs );
                float t0a[3], t1a[3], t0b[3], t1b[3], ha[3], hb[3];
                SpanTangent( s,     e,             t0a );
                SpanTangent( s + 1, e,             t1a );
                SpanTangent( s,     ( e + 1 ) % n, t0b );
                SpanTangent( s + 1, ( e + 1 ) % n, t1b );
                ArcHandle( RingP( st, e ), t0a, RingP( nx, e ), t1a, arcFrom, ha );
                ArcHandle( RingP( st, ( e + 1 ) % n ), t0b,
                           RingP( nx, ( e + 1 ) % n ), t1b, arcFrom, hb );
                for ( int row = 0; row < KLOFT_CURVE_ROWS; ++row )
                {
                    const int   dst = flip ? ( KLOFT_CURVE_ROWS - 1 - row ) : row;
                    const float f   = (float)row / (float)( KLOFT_CURVE_ROWS - 1 );
                    for ( int k = 0; k < 3; ++k )
                        p->ctrl[col + 1][dst].xyz[k] = ha[k] + ( hb[k] - ha[k] ) * f;
                }
            }
        }

        void ApplyCurve()
        {
            if ( !Sel_BrushLive( m_a ) || !m_a->def || !m_a->owner || !m_a->owner->def )
            {
                Sys_Printf( "Loft: the source solid went away — nothing was created.\n" );
                return;
            }
            // Repeat the shared structural gate defensively; do not diverge from preview.
            const char *curveWhy = "invalid geometry";
            if ( !CurveIsBuildable( &curveWhy ) )
            {
                Sys_Printf( "Loft: refused — %s.  Nothing was created.\n", curveWhy );
                return;
            }
            const int segs = (int)m_stations.size() - 1;
            const int n    = RingN( m_stations[0] );

            entity_s      *owner = m_a->owner;
            const brush_t *adef  = m_a->def;

            // Deselect before opening the undo bracket; new patches land selected.
            Select_Deselect( 1 );
            KiwiCmd_UndoBegin( "loft (curve)" );

            int made    = 0;
            int dropped = 0;
            for ( int e = 0; e < n; ++e )
            {
                // Continue the face adjacent to this profile edge; fall back to face A.
                int fi = ( e < (int)m_edgeFace.size() ) ? m_edgeFace[(size_t)e] : -1;
                if ( fi < 0 || fi >= adef->faceCount )
                    fi = m_aFace;
                const face_t *src = &adef->faces[fi];

                // Split into bounded chunks with shared stations and global tangents.
                // Interior visibility emits a coincident row-mirrored second copy.
                const int copies = m_twoSided ? 2 : 1;
                for ( int chunkStart = 0; chunkStart < segs;
                      chunkStart += KLOFT_CURVE_SPANS_PER_PATCH )
                {
                    const int spans = ( segs - chunkStart < KLOFT_CURVE_SPANS_PER_PATCH )
                                    ? ( segs - chunkStart ) : KLOFT_CURVE_SPANS_PER_PATCH;
                    const int w     = spans * 2 + 1;
                    for ( int copy = 0; copy < copies; ++copy )
                    {
                        patchMesh_t *p = MakeNewPatch();
                        if ( !p )
                        {
                            ++dropped;
                            continue;
                        }
                        p->width  = w;
                        p->height = KLOFT_CURVE_ROWS;
                        p->type   = PATCH_BEVEL;  // a swept quadratic, same as the fillet arc

                        p->contents = src->contents;
                        p->flags    = src->toolflags;

                        // Base copy uses outward row order; the optional copy uses its mirror.
                        WriteCurveStrip( p, e, n, segs, chunkStart, spans,
                                         m_outFlip != ( copy != 0 ) );

                        // Preserve the patchfillet material/realize/naturalize order.
                        // CAP alignment suits this swept quadratic rather than a flat cap.
                        p->texture  = *(patchMesh_material *)&src->mtldef[0].lyrMtl;
                        p->lightmap = *(patchMesh_material *)&src->mtldef[1].lyrMtl;
                        KiwiMtl_RealizePatch( p );
                        // Copying can propagate dead material channels; repair them.
                        KiwiMtl_EnsurePatchChannels( p );   // kiwi_material.h:250
                        Patch_KiwiFinishNewLike( p, &src->mtldef[0].mat_texDef );

                        brush_t    *pdef = AddBrushForPatch( p, (entity_s *)owner->def );
                        // ORDERING: AddBrushForPatch must create pSymbiot before CAP
                        // alignment reads its reference face (pmesh.cpp:869).
                        Patch_KiwiCapAlign( p );
                        selbrush_t *inst = Brush_AddToList( pdef, owner );
                        Brush_AddToList2( inst );
                        ++made;
                    }
                }
            }

            if ( dropped )
                Sys_Printf( "Loft: %i patch(es) could not be allocated and were "
                            "skipped — out of patch handles.\n", dropped );

            // Do not leave an empty undo record when every allocation failed.
            if ( made == 0 )
                KiwiCmd_UndoCancel();

            Sel_Clear( KiwiSel() );
            Sel_RebuildFromLegacy();

            Sys_Printf( "Loft: CURVE bridge — %i patch(es) over a %i-gon, %i "
                        "span(s), continuity %s/%s%s.  Patches are RENDER SURFACES: "
                        "they have no collision, so put a caulk brush inside the "
                        "curve if the player has to hit it.\n",
                        made, n, segs, ContName( m_cont[0] ), ContName( m_cont[1] ),
                        m_twoSided ? ", INTERIOR VISIBLE (each strip emitted twice)" : "" );
        }

        void UpdateHud()
        {
            if ( m_stage != KLOFT_LIVE )
            {
                _snprintf( m_hud, sizeof( m_hud ), "loft  %s",
                           m_hover ? "click this face to bridge to"
                                   : "click the other face" );
            }
            else if ( !m_ok )
            {
                _snprintf( m_hud, sizeof( m_hud ), "loft  INVALID  %s",
                           m_why && m_why[0] ? m_why : "cannot bridge these faces" );
            }
            else
            {
                // Report fallback and patch settings before commit to match preview.
                char curveBits[64];
                curveBits[0] = '\0';
                if ( UsePatches() )
                    _snprintf( curveBits, sizeof( curveBits ), "  %s/%s%s",
                               ContName( m_cont[0] ), ContName( m_cont[1] ),
                               m_twoSided ? "  2-sided" : "" );
                curveBits[sizeof( curveBits ) - 1] = '\0';
                _snprintf( m_hud, sizeof( m_hud ),
                           "loft  %s%s%s  density %i  tension %.2f  twist %+i  %i-gon",
                           m_fellBack ? "RULED (tangent refused)" : BridgeName(),
                           UsePatches() ? "  no collision" : "", curveBits,
                           (int)m_stations.size() - 1, m_tension, m_twist,
                           m_stations.empty() ? 0 : RingN( m_stations[0] ) );
            }
            m_hud[sizeof( m_hud ) - 1] = '\0';
        }

        selbrush_t *m_a = 0, *m_b = 0, *m_hover = 0;
        int         m_aFace = -1, m_bFace = -1, m_hoverFace = -1;
        stage_t     m_stage      = KLOFT_PICK_B;
        int         m_bridge     = KLOFT_BRIDGE_RULED;
        int         m_segs       = KLOFT_DEF_SEGS_RULED;
        bool        m_segsTyped  = false;
        float       m_tension    = KLOFT_DEF_TENSION;
        int         m_twist      = 0;      // whole-vertex correspondence steps
        // Curve settings persist; continuity indices are start/face A and end/face B.
        int         m_cont[2]    = { KLOFT_CONT_G1, KLOFT_CONT_G1 };
        bool        m_twoSided   = true;
        // Source-face tangents, both stored forward along A->B.
        float       m_endTan[2][3] = { { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 1.0f } };
        bool        m_haveEndTan = false;
        // Per-build row reversal that makes the base curve strip face outward.
        bool        m_outFlip    = false;
        bool        m_ok         = false;
        bool        m_fellBack   = false;  // tangent -> ruled
        const char *m_why        = "no loft yet";

        std::vector<ring_t> m_stations;    // segs + 1 rings, world space, planar
        std::vector<float>  m_planeN;      // 3 floats per station
        std::vector<float>  m_planeD;      // 1 float  per station
        std::vector<int>    m_edgeFace;    // brush-A face per profile edge
        char                m_hud[192] = { 0 };
    };

    KiwiLoftCommand s_loft;
}

bool KiwiLoft_CanExecute()
{
    const selection_t &sel = KiwiSel();
    if ( sel.active.kind == SEL_FACE && Sel_BrushLive( sel.active.brush )
      && UsableFace( sel.active.brush, sel.active.faceIndex ) )
        return true;
    for ( size_t i = 0; i < sel.items.size(); ++i )
    {
        const sel_item_t &it = sel.items[i];
        if ( it.kind == SEL_FACE && Sel_BrushLive( it.brush )
          && UsableFace( it.brush, it.faceIndex ) )
            return true;
    }
    return false;
}

void KiwiLoft_RegisterCommands()
{
    // Classic registration is unbound; the modern profile binds bare L in kiwi_keymap.cpp.
    Radiant_RegisterCommand( "KiwiLoft", 0, 0, KIWI_CMD_LOFT );
}

KiwiEditorCommand *KiwiLoft_CommandForId( int commandId )
{
    if ( commandId == KIWI_CMD_LOFT )
        return &s_loft;
    return 0;
}

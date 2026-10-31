#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Region and face extrusion build brushes; analytic circles can also build curved
// patches. This file owns profile math, cursor mapping, preview, and validation.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"        // camera_s
// Filled previews use the same triangle-command path as region and split overlays.
#include <gfx_d3d/r_gfx.h>          // GfxColor, Byte4PackPixelColor
#include <gfx_d3d/r_material.h>     // Material
#include <gfx_d3d/r_rendercmds.h>   // MaterialTechniqueType, TECHNIQUE_UNLIT,
                                    // R_AddRenderCmdDrawTris, R_AddCmdSetMaterialColor

#include "kiwi_extrude.h"
#include "kiwi_boxselect.h"             // KiwiBox_ClickSelectAt
#include "kiwi_camera.h"                // KiwiCam_AxisPortrayable
#include "kiwi_command.h"
#include "kiwi_construct.h"
#include "kiwi_grid.h"
#include "kiwi_hover.h"                 // hovered-face arm
#include "kiwi_lines.h"
#include "kiwi_lollipop.h"              // KiwiLollipop_FaceSide (the handle's display side)
#include "kiwi_numeric.h"
#include "kiwi_pick.h"
#include "kiwi_ported.h"
#include "kiwi_primitive.h"             // shared curved-cylinder construction
#include "kiwi_region.h"
#include "kiwi_selection.h"             // selected-face arm
#include "kiwi_snap.h"
#include "kiwi_transform.h"             // KiwiXform_PushFaceOnce (negative E)
#include "kiwi_units.h"
#include "kiwi_validity.h"
#include "kiwi_vec.h"     // Dot3/Sub3/...

#include <algorithm>    // std::sort (face-profile convex hull)
#include <math.h>
#include <stdarg.h>
#include <stdint.h>     // uint16_t index buffers
#include <stdio.h>
#include <stdlib.h>     // getenv (failure dump path)
#include <string.h>
#include <utility>      // std::swap (reversed-winding retry)
#include <vector>

// Ported entry points; addresses identify mirrored ordering and ABI contracts.
extern int Radiant_ProfileGetInt( const char *, const char *, int );
extern bool Radiant_ProfileSetInt( const char *, const char *, int );

// Poll the physical Ctrl key like the ported drag paths; command state cannot
// safely latch a key whose messages are handled elsewhere.
bool KiwiExt_AbsoluteHeld()
{
    return KiwiCmd_CtrlHeld();              // physical, or the test modes' injected state
}

// One-axis snap ladder: aimed named geometry, face-plane magnet, then hard lattice.
// Geometry must precede the lattice, whose nearest answer is always within half a cell.
float KiwiExt_LadderDepth( const snap_result_t &snap, const float *ref,
                           const float *axis, float rawAbs, bool *outMajor )
{
    if ( outMajor )
        *outMajor = false;
    if ( !ref || !axis )
        return rawAbs;

    // Resolve geometry onto this gesture's axis and reject source-plane depth.
    float sd = 0.0f;
    const bool haveGeom = snap.valid && KiwiSnap_IsGeometry( snap.type )
                       && KiwiSnap_AxisDepth( snap, ref, axis, &sd )
                       && fabsf( sd ) >= KEXT_SELF_SNAP_BAND;
    if ( haveGeom )
    {
        if ( snap.type != SNAP_FACE )
            return sd;                     // a place the user aimed at: never rounded

        float at[3];
        for ( int k = 0; k < 3; ++k )
            at[k] = ref[k] + axis[k] * rawAbs;
        // Area hits are magnets near the cursor depth, not remote teleports.
        if ( KiwiSnap_AreaMagnet( rawAbs, sd, at ) == sd )
            return sd;
    }

    // Final rung: absolute world-axis lattice with major-line preference.
    bool major = false;
    const float ld = KiwiSnap_LatticeAxis( rawAbs, ref, axis, true, &major, true );
    if ( outMajor )
        *outMajor = major;
    return ld;
}

// brush.cpp forwarder used by the ported creation sequence.
extern void Ed_BrushSetFaceCount( brush_t *def, int faceCount );

namespace
{
    // Static because the numeric layer copies the field but not its label string.
    const kiwiNumField_t KEXT_FIELDS[1] = { { "length", KNUM_LENGTH, false } };
    const kiwiNumField_t KEXT_CIRCLE_FIELDS[2] = {
        { "length", KNUM_LENGTH, false }, { "sides", KNUM_COUNT, false }
    };
    const kiwiNumField_t KEXT_PATCH_FIELDS[3] = {
        { "length", KNUM_LENGTH, false }, { "density", KNUM_COUNT, false },
        { "wall thickness", KNUM_LENGTH, false }
    };

    const float KEXT_COL_PREVIEW[3] = { 0.55f, 0.85f, 1.00f };   // the prism outline
    const float KEXT_COL_BAD[3]     = { 1.00f, 0.30f, 0.25f };
    // Carve is an ordinary reshape; doomed uses the same warning red as push-through delete.
    const float KEXT_COL_CARVE[3]   = { 1.00f, 0.72f, 0.35f };
    const float KEXT_COL_DOOMED[3]  = { 0.60f, 0.13f, 0.13f };

    // The preview and commit must compare the same exported push-through epsilon.
    const float KEXT_PUSH_EPS = KXPUSH_EPS;

    enum kiwiExtrudeFaceMode_t
    {
        KEXTF_IDLE = 0,   // |distance| too small to mean anything
        KEXTF_GROW,       // OUT: a new body off the face winding (the original verb)
        KEXTF_CARVE,      // IN : push the SOURCE face inward — the brush shrinks
        KEXTF_DESTROY,    // IN, all the way through: the brush is removed
    };



    // `loop` is CCW plane-space data; offsets are world units along plane.normal.
    // Planepts are wound so Face_MakePlane's cross product points outward.
    // Sorting signed endpoints into lo/hi keeps one side-face winding rule.
    struct prismBuild_t
    {
        kconPlane_t plane;
        float       lo, hi;              // offsets along plane.normal
    };

    void PrismVertex( const prismBuild_t &pb, const float uv[2], bool high, float out[3] )
    {
        float base[3];
        KiwiCon_PlaneToWorld( pb.plane, uv, base );
        Mad3( base, pb.plane.normal, high ? pb.hi : pb.lo, out );
    }

    // Filled previews draw translucent side quads and the moving cap before their wireframe.
    // kiwi_lines forces opaque alpha, so surfaces use the render-command triangle path.
    // `idx` is mutable because every triangle is eye-oriented against backface culling.
    void EmitFilledTris( float *xyzw, int vertCount,
                         uint16_t *idx, int idxCount, const float rgba[4] )
    {
        static const float s_neutral[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        static const float s_white[4]   = { 1.0f, 1.0f, 1.0f, 1.0f };

        const camera_s *cam = Ed_Camera();
        if ( !cam || vertCount < 3 || idxCount < 3 )
            return;

        enum { KEXT_PREVIEW_MAX_VERTS = 4 * ( KEXT_MAX_PROFILE + 1 ) };
        if ( vertCount > KEXT_PREVIEW_MAX_VERTS )
            return;

        static float nrm[KEXT_PREVIEW_MAX_VERTS][3];
        static float st [KEXT_PREVIEW_MAX_VERTS][2];
        static float col[KEXT_PREVIEW_MAX_VERTS];

        float rgbaCopy[4] = { rgba[0], rgba[1], rgba[2], rgba[3] };
        GfxColor packed;
        Byte4PackPixelColor( rgbaCopy, &packed );
        const float packedAsFloat = *(float *)&packed.packed;   // bit-cast, as the ported batcher does

        for ( int i = 0; i < vertCount; ++i )
        {
            // Use a constant world normal: a camera vector would make brightness view-dependent.
            KiwiTris_FillNormal( nrm[i] );
            st[i][0]  = 0.0f;
            st[i][1]  = 0.0f;
            col[i]    = packedAsFloat;
        }

        // Eye-orient each triangle so both halves of the closed prism survive backface culling.
        KiwiTris_OrientToEye( xyzw, 4, idx, idxCount, cam->origin );

        R_AddCmdSetMaterialColor( s_neutral );
        R_AddRenderCmdDrawTris( g_qeglobals.d_white, TECHNIQUE_UNLIT,
                                (short)idxCount, idx, (short)vertCount,
                                (float (*)[4])xyzw, nrm, col, st );
        R_AddCmdSetMaterialColor( s_white );
    }

    // `loopUV` is in pb.plane coordinates; capTris indexes that profile.
    // Cap and sides use separate draws so the moving cap can be more prominent.
    // The white material backface-culls, so EmitFilledTris eye-orients each triangle.
    // `slit` (optional): a ring's bridge edges (kregion_t::bridge) - no side wall there.
    void EmitPrismSolid( const prismBuild_t &pb, const std::vector<float> &loopUV,
                         const std::vector<int> &capTris, const float rgb[3],
                         const std::vector<unsigned char> *slit = nullptr )
    {
        const int n = (int)( loopUV.size() / 2 );
        if ( n < 3 || n > KEXT_MAX_PROFILE )
            return;
        if ( !( pb.hi - pb.lo > 1.0e-4f ) )
            return;                          // no depth: there is no solid to show

        // The sign chooses the moving cap; MakeBuild places the source at the other endpoint.
        const bool capHigh = !( pb.lo < 0.0f );
        {
            static float xyzw[KEXT_MAX_PROFILE][4];
            static uint16_t idx[( KEXT_MAX_PROFILE - 2 ) * 3];
            if ( (int)capTris.size() >= 3
              && (int)capTris.size() <= ( KEXT_MAX_PROFILE - 2 ) * 3 )
            {
                for ( int i = 0; i < n; ++i )
                {
                    float w[3];
                    PrismVertex( pb, &loopUV[(size_t)i * 2], capHigh, w );
                    xyzw[i][0] = w[0]; xyzw[i][1] = w[1]; xyzw[i][2] = w[2];
                    xyzw[i][3] = 1.0f;
                }
                for ( size_t k = 0; k < capTris.size(); ++k )
                    idx[k] = (uint16_t)capTris[k];
                const float capRgba[4] = { rgb[0], rgb[1], rgb[2], 0.34f };
                EmitFilledTris( &xyzw[0][0], n, idx, (int)capTris.size(), capRgba );
            }
        }

        // Batch all side quads to avoid reopening material-color state per edge.
        {
            static float    xyzw[4 * KEXT_MAX_PROFILE][4];
            static uint16_t idx [6 * KEXT_MAX_PROFILE];
            int v = 0, t = 0;
            for ( int i = 0; i < n; ++i )
            {
                if ( slit && i < (int)slit->size() && ( *slit )[(size_t)i] )
                    continue;
                const int j = ( i + 1 ) % n;
                float p[4][3];
                PrismVertex( pb, &loopUV[(size_t)i * 2], false, p[0] );
                PrismVertex( pb, &loopUV[(size_t)j * 2], false, p[1] );
                PrismVertex( pb, &loopUV[(size_t)j * 2], true,  p[2] );
                PrismVertex( pb, &loopUV[(size_t)i * 2], true,  p[3] );
                const int base = v;
                for ( int k = 0; k < 4; ++k, ++v )
                {
                    xyzw[v][0] = p[k][0]; xyzw[v][1] = p[k][1]; xyzw[v][2] = p[k][2];
                    xyzw[v][3] = 1.0f;
                }
                idx[t++] = (uint16_t)( base + 0 );
                idx[t++] = (uint16_t)( base + 1 );
                idx[t++] = (uint16_t)( base + 2 );
                idx[t++] = (uint16_t)( base + 0 );
                idx[t++] = (uint16_t)( base + 2 );
                idx[t++] = (uint16_t)( base + 3 );
            }
            const float sideRgba[4] = { rgb[0], rgb[1], rgb[2], 0.20f };
            EmitFilledTris( &xyzw[0][0], v, idx, t, sideRgba );
        }
    }

    // Wire helper for a complete prism: both cap rings and every vertical.
    void EmitPrismWire( const prismBuild_t &pb, const std::vector<float> &loopUV )
    {
        const int n = (int)( loopUV.size() / 2 );
        for ( int i = 0; i < n; ++i )
        {
            const int j = ( i + 1 ) % n;
            float a[3], b[3];
            PrismVertex( pb, &loopUV[(size_t)i * 2], true,  a );
            PrismVertex( pb, &loopUV[(size_t)j * 2], true,  b );
            if ( !KiwiLines_Add( a, b ) )
                return;
            PrismVertex( pb, &loopUV[(size_t)i * 2], false, a );
            PrismVertex( pb, &loopUV[(size_t)j * 2], false, b );
            if ( !KiwiLines_Add( a, b ) )
                return;
            PrismVertex( pb, &loopUV[(size_t)i * 2], false, a );
            PrismVertex( pb, &loopUV[(size_t)i * 2], true,  b );
            if ( !KiwiLines_Add( a, b ) )
                return;
        }
    }

    inline float Cross2( const float *o, const float *a, const float *b )
    {
        return ( a[0] - o[0] ) * ( b[1] - o[1] ) - ( a[1] - o[1] ) * ( b[0] - o[0] );
    }

    // Pick well-spread cap planepts in ascending ring order. Ascending order preserves
    // CCW winding; spread avoids near-collinear triples on dense profiles.
    bool CapTriple( const std::vector<float> &loopUV, const std::vector<int> &ring,
                    int *o0, int *o1, int *o2 )
    {
        const int n = (int)ring.size();
        if ( n < 3 )
            return false;
        const float *p0 = &loopUV[(size_t)ring[0] * 2];

        int   i1 = -1;
        float best = 0.0f;
        for ( int k = 1; k < n; ++k )
        {
            const float *p = &loopUV[(size_t)ring[k] * 2];
            const float d = ( p[0] - p0[0] ) * ( p[0] - p0[0] )
                          + ( p[1] - p0[1] ) * ( p[1] - p0[1] );
            if ( i1 < 0 || d > best ) { best = d; i1 = k; }
        }
        if ( i1 < 0 || best < 1.0e-6f )
            return false;

        int   i2 = -1;
        float bestArea = 0.0f;
        const float *p1 = &loopUV[(size_t)ring[i1] * 2];
        for ( int k = 1; k < n; ++k )
        {
            if ( k == i1 )
                continue;
            const float a = fabsf( Cross2( p0, p1, &loopUV[(size_t)ring[k] * 2] ) );
            if ( i2 < 0 || a > bestArea ) { bestArea = a; i2 = k; }
        }
        if ( i2 < 0 || bestArea < 1.0e-4f )
            return false;                    // every vertex is collinear — no plane

        int t[3] = { 0, i1, i2 };
        for ( int a = 0; a < 2; ++a )
            for ( int b = a + 1; b < 3; ++b )
                if ( t[b] < t[a] ) { const int s = t[a]; t[a] = t[b]; t[b] = s; }
        *o0 = t[0]; *o1 = t[1]; *o2 = t[2];
        return true;
    }

    // Remove zero-length and same-direction collinear edges before writing side planes;
    // otherwise they produce degenerate or duplicate planes. Use the validity gate's
    // tolerance and iterate to a fixed point because removal can expose another pair.
    // This also protects non-region callers of the exported prism builder.
    void CleanRing( const std::vector<float> &loopUV, std::vector<int> *ring )
    {
        bool changed = true;
        while ( changed )
        {
            changed = false;
            const int n = (int)ring->size();
            if ( n < 4 )
                return;
            for ( int i = 0; i < n; ++i )
            {
                const float *a = &loopUV[(size_t)( *ring )[( i + n - 1 ) % n] * 2];
                const float *b = &loopUV[(size_t)( *ring )[i] * 2];
                const float *c = &loopUV[(size_t)( *ring )[( i + 1 ) % n] * 2];
                const float e0[2] = { b[0] - a[0], b[1] - a[1] };
                const float e1[2] = { c[0] - b[0], c[1] - b[1] };
                const float l0 = sqrtf( e0[0] * e0[0] + e0[1] * e0[1] );
                const float l1 = sqrtf( e1[0] * e1[0] + e1[1] * e1[1] );
                bool drop = false;
                if ( !( l1 > 1.0e-4f ) )
                    drop = true;             // b == c: a zero-length side face
                else if ( l0 > 1.0e-4f )
                    drop = ( ( e0[0] * e1[0] + e0[1] * e1[1] ) / ( l0 * l1 ) )
                           > KVALID_PLANE_DOT;
                if ( !drop )
                    continue;
                ring->erase( ring->begin() + i );
                changed = true;
                break;
            }
        }
    }

    // Build one convex piece as an unlinked def; callers land it only after all gates pass.
    brush_t *BuildPieceDef( const prismBuild_t &pb, const std::vector<float> &loopUV,
                            const std::vector<int> &ringIn, const char **why )
    {
        // Clean a copy so the caller's decomposition still matches its preview.
        std::vector<int> ring = ringIn;
        CleanRing( loopUV, &ring );
        const int n = (int)ring.size();
        if ( n < 3 )
        {
            *why = "piece has fewer than 3 vertices";
            return 0;
        }
        if ( n > KEXT_MAX_PROFILE )
        {
            *why = "piece over the profile cap";
            return 0;
        }

        int i0, i1, i2;
        if ( !CapTriple( loopUV, ring, &i0, &i1, &i2 ) )
        {
            *why = "piece is degenerate (collinear)";
            return 0;
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

        // Reverse the CCW triple so the high-cap normal is +plane.normal.
        {
            face_t *f = &def->faces[0];
            PrismVertex( pb, &loopUV[(size_t)ring[i2] * 2], true, f->planepts[0] );
            PrismVertex( pb, &loopUV[(size_t)ring[i1] * 2], true, f->planepts[1] );
            PrismVertex( pb, &loopUV[(size_t)ring[i0] * 2], true, f->planepts[2] );
        }
        // The CCW triple gives the low cap a -plane.normal normal.
        {
            face_t *f = &def->faces[1];
            PrismVertex( pb, &loopUV[(size_t)ring[i0] * 2], false, f->planepts[0] );
            PrismVertex( pb, &loopUV[(size_t)ring[i1] * 2], false, f->planepts[1] );
            PrismVertex( pb, &loopUV[(size_t)ring[i2] * 2], false, f->planepts[2] );
        }

        // Each side uses the full edge and extrusion spans to keep planepts well separated.
        // For a CCW ring in a right-handed (u,v,normal) basis, (b_lo,a_lo,a_hi)
        // makes Face_MakePlane point outward as u*dv - v*du.
        for ( int e = 0; e < n; ++e )
        {
            const float *a = &loopUV[(size_t)ring[e] * 2];
            const float *b = &loopUV[(size_t)ring[( e + 1 ) % n] * 2];
            face_t *f = &def->faces[2 + e];
            PrismVertex( pb, b, false, f->planepts[0] );
            PrismVertex( pb, a, false, f->planepts[1] );
            PrismVertex( pb, a, true,  f->planepts[2] );
        }
        return def;
    }

    // Ported Ed_NewBrushDrag tail order (xywnd.cpp 0x467fa0).
    selbrush_t *LandDef( brush_t *def )
    {
        Entity_LinkBrush( def, (entity_s *)world_entity->def );
        selbrush_t *inst = Brush_AddToList( def, world_entity );
        Brush_AddToList2( inst );                 // → selected_brushes
        return inst;
    }

    void EmitCylinderRims( const prismBuild_t &pb, const std::vector<float> &outer,
                           const std::vector<float> &inner, const float rgb[3] )
    {
        const int n = (int)outer.size()/2;
        if ( n < 3 || n > KEXT_MAX_PROFILE || inner.size() != outer.size() ) return;
        float xyzw[4*KEXT_MAX_PROFILE][4];
        uint16_t indices[6*KEXT_MAX_PROFILE];
        const float color[4] = {rgb[0],rgb[1],rgb[2],0.34f};
        for ( int end = 0; end < 2; ++end )
        {
            for ( int i = 0; i < n; ++i )
            {
                const int j = (i+1)%n;
                const float *uv[4] = { &outer[i*2], &outer[j*2], &inner[j*2], &inner[i*2] };
                for ( int v = 0; v < 4; ++v )
                {
                    PrismVertex( pb, uv[v], end != 0, xyzw[i*4+v] );
                    xyzw[i*4+v][3] = 1;
                }
                const int order[6] = {0,1,2,0,2,3};
                for ( int k = 0; k < 6; ++k ) indices[i*6+k] = (uint16_t)(i*4+order[k]);
                KiwiLines_Add( xyzw[i*4], xyzw[i*4+3] );
            }
            EmitFilledTris( &xyzw[0][0], n*4, indices, n*6, color );
        }
    }

    // KIWI (2026-10-31, user: "when im extruding from the Z (flat ground) plane, position should
    // be UPWARDS not downwards"): a region's plane faces whichever way its loop happened to wind,
    // so a ground face often faced down and a positive depth dug.  A flat region facing down is
    // turned to face up for the extrusion: normal and v negated (u x v stays the normal), every
    // plane-space loop mirrored in v and re-wound so outlines stay CCW and holes CW.
    void MirrorLoopV( std::vector<float> &loop )
    {
        const size_t n = loop.size() / 2;
        std::vector<float> out( loop.size() );
        for ( size_t i = 0; i < n; ++i )
        {
            out[i * 2 + 0] =  loop[( n - 1 - i ) * 2 + 0];
            out[i * 2 + 1] = -loop[( n - 1 - i ) * 2 + 1];
        }
        loop.swap( out );
    }

    kregion_t FlipRegion( const kregion_t &reg )
    {
        kregion_t r = reg;
        for ( int k = 0; k < 3; ++k )
        {
            r.plane.normal[k] = -r.plane.normal[k];
            r.plane.v[k]      = -r.plane.v[k];
        }
        MirrorLoopV( r.pts );
        MirrorLoopV( r.outer );
        for ( std::vector<float> &hole : r.holes )
            MirrorLoopV( hole );
        // Reversed, new edge j runs old edge n-2-j backwards.
        const size_t n = reg.pts.size() / 2;
        if ( n > 0 && reg.bridge.size() == n )
            for ( size_t j = 0; j < n; ++j )
                r.bridge[j] = reg.bridge[( 2 * n - 2 - j ) % n];
        return r;
    }

    kregion_t UprightRegion( const kregion_t &reg )
    {
        return ( reg.plane.normal[2] < -0.999f ) ? FlipRegion( reg ) : reg;
    }

    // KIWI (2026-10-31, user: "there's no way to control the number of brush sides on the arc"):
    // a construction face's outline carries an arc's DISPLAY tessellation (up to 64 samples per
    // full circle).  Runs of it that lie on a visible construction circle / arc in the face's
    // plane - consecutive points on it at most about a sample apart - are resampled at `sides`
    // per full circle, prorated by the run's sweep, ends kept: the count a whole circle extrudes
    // with.  A straight line joining two points of the circle spans more than a sample and stays.
    struct roundRef_t
    {
        float c[2];     // centre, plane space
        float r;
        float step;     // the object's sample spacing, radians
    };

    void RoundsInPlane( const kconPlane_t &pl, std::vector<roundRef_t> *out )
    {
        out->clear();
        for ( int i = 0; i < KiwiCon_Count(); ++i )
        {
            const kconObject_t *o = KiwiCon_At( i );
            if ( !o || o->hidden || ( o->type != KCON_CIRCLE && o->type != KCON_ARC ) || !( o->radius > 0.0f ) )
                continue;
            if ( fabsf( Dot3( o->plane.normal, pl.normal ) ) < 0.9999f )
                continue;
            float w[3], rel[3];
            KiwiCon_PlaneToWorld( o->plane, o->centre, w );
            Sub3( w, pl.origin, rel );
            if ( fabsf( Dot3( rel, pl.normal ) ) > KCON_PLANE_FIT_DIST )
                continue;
            roundRef_t rr;
            KiwiCon_WorldToPlane( pl, w, rr.c );
            rr.r = o->radius;
            const int   verts = KiwiCon_VertCount( *o );
            const int   segs  = ( o->type == KCON_CIRCLE ) ? verts : verts - 1;
            const float sweep = ( o->type == KCON_CIRCLE ) ? 360.0f : fabsf( o->ang1 - o->ang0 );
            rr.step = ( segs > 0 ? sweep / (float)segs : 360.0f ) * KCON_DEG2RAD;
            out->push_back( rr );
        }
    }

    // One curve run handed to KiwiPrim_BuildCurvePatches: its quadratic controls (plane-space
    // u,v pairs, odd count, ends on the loop), in the loop's CCW order.
    struct curveRun_t
    {
        std::vector<float> ctrl;
    };

    // An arc as non-rational quadratic spans of at most 30 degrees, each middle control on the
    // tangent intersection at r / cos(half-span) (kiwi_patchfillet.cpp's fillet).
    void ArcControls( const float c[2], float r, float a0, float sweep, std::vector<float> *ctrl )
    {
        const float kSpan = 30.0f * KCON_DEG2RAD;
        int spans = (int)ceilf( fabsf( sweep ) / kSpan - 1.0e-3f );
        if ( spans < 1 )
            spans = 1;
        const float step = sweep / (float)spans;
        ctrl->clear();
        for ( int col = 0; col <= spans * 2; ++col )
        {
            const float a  = a0 + step * (float)col * 0.5f;
            const float rr = ( col & 1 ) ? r / cosf( step * 0.5f ) : r;
            ctrl->push_back( c[0] + cosf( a ) * rr );
            ctrl->push_back( c[1] + sinf( a ) * rr );
        }
    }

    // KIWI (2026-10-31, user: "make it so this mixture of spline/arc/line extrudes the spline
    // portion also with a curve in P mode"): a spline is Catmull-Rom (cubic), which no quadratic
    // patch holds exactly, so its tessellated run is fitted.  Spans take at most 30 degrees of
    // turn, like the arcs; each middle control sits on the intersection of the run's tangents at
    // the span's ends, so neighbouring spans share a tangent (no crease).  Where those tangents
    // do not meet ahead of both ends (an inflection, a straight stretch) the span instead passes
    // through the run's middle point.
    void FitQuadraticRun( const std::vector<float> &run, std::vector<float> *ctrl )
    {
        const int m = (int)( run.size() / 2 ) - 1;          // edges
        auto P = [&]( int i ) { return &run[(size_t)i * 2]; };
        std::vector<float> tan( (size_t)( m + 1 ) * 2 );
        for ( int i = 0; i <= m; ++i )
        {
            const float *a = P( i > 0 ? i - 1 : 0 ), *b = P( i < m ? i + 1 : m );
            float dx = b[0] - a[0], dy = b[1] - a[1];
            const float len = sqrtf( dx * dx + dy * dy );
            if ( len > 1.0e-6f ) { dx /= len; dy /= len; }
            tan[(size_t)i * 2] = dx;
            tan[(size_t)i * 2 + 1] = dy;
        }
        auto edgeAngle = [&]( int i ) {                      // direction of edge i -> i + 1
            return atan2f( P( i + 1 )[1] - P( i )[1], P( i + 1 )[0] - P( i )[0] );
        };
        const float kSpan = 30.0f * KCON_DEG2RAD;
        ctrl->assign( P( 0 ), P( 0 ) + 2 );
        int a = 0;
        while ( a < m )
        {
            int b = a + 1;
            float turn = 0.0f;
            while ( b < m )
            {
                float d = edgeAngle( b ) - edgeAngle( b - 1 );
                while ( d >  KCON_PI ) d -= 2.0f * KCON_PI;
                while ( d < -KCON_PI ) d += 2.0f * KCON_PI;
                if ( turn + fabsf( d ) > kSpan )
                    break;
                turn += fabsf( d );
                ++b;
            }
            const float *p0 = P( a ), *p2 = P( b );
            const float *t0 = &tan[(size_t)a * 2], *t2 = &tan[(size_t)b * 2];
            const float cx = p2[0] - p0[0], cy = p2[1] - p0[1];
            const float chord = sqrtf( cx * cx + cy * cy );
            // p0 + s t0 = p2 - u t2
            const float den = t0[0] * t2[1] - t0[1] * t2[0];
            float mid[2];
            bool met = false;
            if ( fabsf( den ) > 1.0e-4f )
            {
                const float s = ( cx * t2[1] - cy * t2[0] ) / den;
                const float u = ( cx * t0[1] - cy * t0[0] ) / den;
                if ( s > 0.0f && u > 0.0f && s < chord && u < chord )
                {
                    mid[0] = p0[0] + t0[0] * s;
                    mid[1] = p0[1] + t0[1] * s;
                    met = true;
                }
            }
            if ( !met )
            {
                // B(1/2) = (p0 + 2 p1 + p2) / 4 through the run's middle point.
                const float *pm = P( ( a + b ) / 2 );
                float on[2] = { pm[0], pm[1] };
                if ( b - a == 1 ) { on[0] = ( p0[0] + p2[0] ) * 0.5f; on[1] = ( p0[1] + p2[1] ) * 0.5f; }
                mid[0] = 2.0f * on[0] - ( p0[0] + p2[0] ) * 0.5f;
                mid[1] = 2.0f * on[1] - ( p0[1] + p2[1] ) * 0.5f;
            }
            ctrl->push_back( mid[0] );
            ctrl->push_back( mid[1] );
            ctrl->push_back( p2[0] );
            ctrl->push_back( p2[1] );
            a = b;
        }
    }

    // True when the CCW loop's run from point i to point j (`run`, inclusive) can be cut down to
    // its chord with a flat cap: it bulges out (right of the chord, never left) and runs along the
    // chord without doubling back, so the cap - the run projected onto its chord - never folds.
    bool BulgesOutAlongChord( const std::vector<float> &run )
    {
        const int n = (int)( run.size() / 2 );
        const float *a = &run[0], *b = &run[(size_t)( n - 1 ) * 2];
        float dx = b[0] - a[0], dy = b[1] - a[1];
        const float len = sqrtf( dx * dx + dy * dy );
        if ( n < 3 || !( len > 1.0e-3f ) )
            return false;
        dx /= len;
        dy /= len;
        const float tol = 1.0e-3f * len > 0.01f ? 1.0e-3f * len : 0.01f;
        float lastT = 0.0f, deepest = 0.0f;
        for ( int i = 1; i < n; ++i )
        {
            const float rx = run[(size_t)i * 2] - a[0], ry = run[(size_t)i * 2 + 1] - a[1];
            const float side = dx * ry - dy * rx;            // < 0: right, outside the loop
            const float t = rx * dx + ry * dy;
            if ( side > tol || t < lastT - tol )
                return false;
            if ( -side > deepest )
                deepest = -side;
            if ( t > lastT )
                lastT = t;
        }
        return deepest > tol;
    }

    // Splines in the face's plane: their tessellation in plane space, and for each tessellation
    // edge whether it belongs to a smooth (spline) span - a straight span of a mixed chain stays
    // an ordinary edge.
    struct splineRef_t
    {
        std::vector<float>         tess;      // plane-space u,v pairs
        std::vector<unsigned char> smoothEdge;
        bool                       closed = false;
    };

    void SplinesInPlane( const kconPlane_t &pl, std::vector<splineRef_t> *out )
    {
        out->clear();
        for ( int i = 0; i < KiwiCon_Count(); ++i )
        {
            const kconObject_t *o = KiwiCon_At( i );
            if ( !o || o->hidden || !KiwiCon_HasSmooth( *o ) )
                continue;
            const int verts = KiwiCon_VertCount( *o );
            if ( verts < 2 )
                continue;
            splineRef_t sr;
            sr.closed = o->closed;
            bool onPlane = true;
            std::vector<float> world( (size_t)verts * 3 );
            for ( int v = 0; v < verts && onPlane; ++v )
            {
                float *w = &world[(size_t)v * 3], rel[3], uv[2];
                if ( !KiwiCon_VertWorld( *o, v, w ) )
                {
                    onPlane = false;
                    break;
                }
                Sub3( w, pl.origin, rel );
                if ( fabsf( Dot3( rel, pl.normal ) ) > KCON_PLANE_FIT_DIST )
                    onPlane = false;
                KiwiCon_WorldToPlane( pl, w, uv );
                sr.tess.push_back( uv[0] );
                sr.tess.push_back( uv[1] );
            }
            if ( !onPlane )
                continue;
            // A tessellation edge is straight when it joins two consecutive control points
            // across a span not marked smooth (KiwiCon_TessellateMixed emits only its start).
            const int ctrlN = (int)( o->pts.size() / 3 );
            auto ctrlAt = [&]( const float *w ) {
                for ( int c = 0; c < ctrlN; ++c )
                    if ( Dist3( w, &o->pts[(size_t)c * 3] ) < 1.0e-3f )
                        return c;
                return -1;
            };
            const int edges = sr.closed ? verts : verts - 1;
            sr.smoothEdge.assign( (size_t)edges, 1 );
            for ( int e = 0; e < edges; ++e )
            {
                const int c0 = ctrlAt( &world[(size_t)e * 3] );
                const int c1 = ctrlAt( &world[(size_t)( ( e + 1 ) % verts ) * 3] );
                if ( c0 < 0 || c1 < 0 )
                    continue;
                const int spans = o->closed ? ctrlN : ctrlN - 1;
                for ( int s = 0; s < spans; ++s )
                    if ( c0 == s && c1 == ( s + 1 ) % ctrlN
                      && !( s < (int)o->smooth.size() && o->smooth[(size_t)s] ) )
                        sr.smoothEdge[(size_t)e] = 0;
            }
            out->push_back( sr );
        }
    }

    // Outward spline runs of the loop cut down to their chords, each recorded in `curveRuns`
    // with its fitted controls.  True when any was.
    bool ShapeSplineRuns( std::vector<float> &pts, const kconPlane_t &pl,
                          std::vector<curveRun_t> *curveRuns )
    {
        const int n = (int)( pts.size() / 2 );
        if ( n < 3 )
            return false;
        std::vector<splineRef_t> splines;
        SplinesInPlane( pl, &splines );
        if ( splines.empty() )
            return false;
        // Each loop point's position along each spline's tessellation (edge + fraction), or -1.
        const float tol = 0.05f;
        std::vector<float> param( (size_t)n * splines.size(), -1.0f );
        for ( int i = 0; i < n; ++i )
            for ( size_t k = 0; k < splines.size(); ++k )
            {
                const splineRef_t &sr = splines[k];
                const int verts = (int)( sr.tess.size() / 2 );
                const int edges = (int)sr.smoothEdge.size();
                float best = tol;
                for ( int e = 0; e < edges; ++e )
                {
                    const float *a = &sr.tess[(size_t)e * 2], *b = &sr.tess[(size_t)( ( e + 1 ) % verts ) * 2];
                    const float ex = b[0] - a[0], ey = b[1] - a[1];
                    const float ll = ex * ex + ey * ey;
                    const float px = pts[(size_t)i * 2] - a[0], py = pts[(size_t)i * 2 + 1] - a[1];
                    float t = ll > 1.0e-12f ? ( px * ex + py * ey ) / ll : 0.0f;
                    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
                    const float dx = px - ex * t, dy = py - ey * t;
                    const float d = sqrtf( dx * dx + dy * dy );
                    if ( d < best )
                    {
                        best = d;
                        param[(size_t)i * splines.size() + k] = (float)e + t;
                    }
                }
            }
        // Edge i (point i -> i + 1) follows spline k along a smooth span, or -1.
        std::vector<int> edge( (size_t)n, -1 );
        bool any = false;
        for ( int i = 0; i < n; ++i )
        {
            const int j = ( i + 1 ) % n;
            for ( size_t k = 0; k < splines.size(); ++k )
            {
                const float pi = param[(size_t)i * splines.size() + k];
                const float pj = param[(size_t)j * splines.size() + k];
                if ( pi < 0.0f || pj < 0.0f )
                    continue;
                const int edges = (int)splines[k].smoothEdge.size();
                float d = pj - pi;
                if ( splines[k].closed )
                {
                    if ( d >  0.5f * (float)edges ) d -= (float)edges;
                    if ( d < -0.5f * (float)edges ) d += (float)edges;
                }
                if ( fabsf( d ) < 1.0e-4f || fabsf( d ) > 1.0001f )
                    continue;
                int e = (int)floorf( pi + d * 0.5f );
                e = ( ( e % edges ) + edges ) % edges;
                if ( !splines[k].smoothEdge[(size_t)e] )
                    continue;
                edge[(size_t)i] = (int)k;
                any = true;
                break;
            }
        }
        if ( !any )
            return false;
        int start = -1;
        for ( int i = 0; i < n && start < 0; ++i )
            if ( edge[(size_t)i] != edge[(size_t)( ( i + n - 1 ) % n )] )
                start = i;
        if ( start < 0 )
            return false;                        // the whole loop is one spline: no chord

        std::vector<float> out;
        bool cut = false;
        int i = start, done = 0;
        while ( done < n )
        {
            out.push_back( pts[(size_t)i * 2] );
            out.push_back( pts[(size_t)i * 2 + 1] );
            const int k = edge[(size_t)i];
            int j = i, edges = 0;
            while ( k >= 0 && done + edges < n && edge[(size_t)j] == k )
            {
                j = ( j + 1 ) % n;
                ++edges;
            }
            if ( edges >= 2 )
            {
                std::vector<float> run;
                for ( int s = 0, p = i; s <= edges; ++s, p = ( p + 1 ) % n )
                {
                    run.push_back( pts[(size_t)p * 2] );
                    run.push_back( pts[(size_t)p * 2 + 1] );
                }
                if ( BulgesOutAlongChord( run ) )
                {
                    curveRun_t cr;
                    FitQuadraticRun( run, &cr.ctrl );
                    curveRuns->push_back( cr );
                    cut = true;
                    done += edges;               // the chord: point j comes next
                    i = j;
                    continue;
                }
            }
            i = ( i + 1 ) % n;
            ++done;
        }
        if ( cut )
            pts.swap( out );
        return cut;
    }

    // True when `pts` had an arc run.  Runs are resampled at `sides` per full circle, except an
    // arc bulging OUT of the loop by at most a half circle: that one takes `outSides` - or, with
    // `curveRuns`, is cut down to its chord and recorded there instead, for
    // KiwiPrim_BuildCurvePatches (Extrude's patch mode).  An arc bulging in stays sides: its chord
    // would fill the notch.
    bool ShapeRoundRuns( std::vector<float> &pts, const kconPlane_t &pl, int sides, int outSides,
                         std::vector<curveRun_t> *curveRuns )
    {
        const int n = (int)( pts.size() / 2 );
        if ( n < 3 )
            return false;
        std::vector<roundRef_t> rounds;
        RoundsInPlane( pl, &rounds );
        if ( rounds.empty() )
            return false;
        auto angleOn = [&]( int i, const roundRef_t &rr, float *ang ) {
            const float dx = pts[(size_t)i * 2] - rr.c[0], dy = pts[(size_t)i * 2 + 1] - rr.c[1];
            const float tol = rr.r * 1.0e-3f > 0.02f ? rr.r * 1.0e-3f : 0.02f;
            if ( fabsf( sqrtf( dx * dx + dy * dy ) - rr.r ) > tol )
                return false;
            *ang = atan2f( dy, dx );
            return true;
        };
        auto wrapPi = []( float a ) {
            while ( a >  KCON_PI ) a -= 2.0f * KCON_PI;
            while ( a < -KCON_PI ) a += 2.0f * KCON_PI;
            return a;
        };
        // Edge i (point i -> i + 1) is an arc chord of round k, or -1.
        std::vector<int> edge( (size_t)n, -1 );
        bool any = false;
        for ( int i = 0; i < n; ++i )
        {
            const int j = ( i + 1 ) % n;
            for ( size_t k = 0; k < rounds.size(); ++k )
            {
                float ai, aj;
                if ( !angleOn( i, rounds[k], &ai ) || !angleOn( j, rounds[k], &aj ) )
                    continue;
                const float d = fabsf( wrapPi( aj - ai ) );
                if ( d > 1.0e-6f && d <= rounds[k].step * 1.5f )
                {
                    edge[(size_t)i] = (int)k;
                    any = true;
                    break;
                }
            }
        }
        if ( !any )
            return false;
        // Start where the edge kind changes, so no run wraps round the seam.
        int start = -1;
        for ( int i = 0; i < n && start < 0; ++i )
            if ( edge[(size_t)i] != edge[(size_t)( ( i + n - 1 ) % n )] )
                start = i;
        if ( start < 0 )
            return false;                        // the whole loop is one circle: leave it

        std::vector<float> out;
        int i = start, done = 0;
        while ( done < n )
        {
            out.push_back( pts[(size_t)i * 2] );
            out.push_back( pts[(size_t)i * 2 + 1] );
            const int k = edge[(size_t)i];
            if ( k < 0 )
            {
                i = ( i + 1 ) % n;
                ++done;
                continue;
            }
            const roundRef_t &rr = rounds[(size_t)k];
            float a0 = 0.0f, prev = 0.0f, sweep = 0.0f;
            angleOn( i, rr, &a0 );
            prev = a0;
            int j = i, edges = 0;
            while ( done + edges < n && edge[(size_t)j] == k )
            {
                j = ( j + 1 ) % n;
                float a = prev;
                angleOn( j, rr, &a );
                sweep += wrapPi( a - prev );
                prev = a;
                ++edges;
            }
            int runSides = sides;
            if ( fabsf( sweep ) <= KCON_PI + 1.0e-3f )
            {
                // The loop is CCW, so the outside of chord a -> b is its right: an arc bulging
                // out has its middle there.
                const float *pa = &pts[(size_t)i * 2], *pb = &pts[(size_t)j * 2];
                const float am  = a0 + sweep * 0.5f;
                const float mx = rr.c[0] + cosf( am ) * rr.r - pa[0];
                const float my = rr.c[1] + sinf( am ) * rr.r - pa[1];
                const bool  bulges = ( pb[0] - pa[0] ) * my - ( pb[1] - pa[1] ) * mx < 0.0f;
                if ( bulges )
                    runSides = outSides;
                if ( bulges && curveRuns )
                {
                    curveRun_t run;
                    ArcControls( rr.c, rr.r, a0, sweep, &run.ctrl );
                    // The ends sit exactly on the loop's points, where the brush's chord ends.
                    const size_t last = run.ctrl.size() - 2;
                    run.ctrl[0] = pa[0];  run.ctrl[1] = pa[1];
                    run.ctrl[last] = pb[0];  run.ctrl[last + 1] = pb[1];
                    curveRuns->push_back( run );
                    done += edges;               // the chord: point j comes next
                    i = j;
                    continue;
                }
            }
            int segs = (int)floorf( (float)runSides * fabsf( sweep ) / ( 2.0f * KCON_PI ) + 0.5f );
            if ( segs < 1 )
                segs = 1;
            for ( int s = 1; s < segs; ++s )
            {
                const float a = a0 + sweep * (float)s / (float)segs;
                out.push_back( rr.c[0] + cosf( a ) * rr.r );
                out.push_back( rr.c[1] + sinf( a ) * rr.r );
            }
            done += edges;
            i = j;
        }
        pts.swap( out );
        return true;
    }

    bool ResampleRoundRuns( std::vector<float> &pts, const kconPlane_t &pl, int sides )
    {
        return ShapeRoundRuns( pts, pl, sides, sides, nullptr );
    }

    // Region extrusion command.
    class KiwiExtrudeCommand : public KiwiEditorCommand
    {
    public:
        const char *Name() const override { return "Extrude Region"; }
        bool CanExecute() override { return KiwiExtrude_CanExecute(); }

        const char *HudStatus() const override { return m_hud[0] ? m_hud : 0; }
        bool        HudInvalid() const override { return m_invalid; }

        int NumericFields( const kiwiNumField_t **out ) const override
        { *out = KEXT_FIELDS; return 1; }

        int CommandOptions( const kiwiOption_t **out ) const override
        {
            if ( !m_patchMode ) return 0;
            if ( !m_hasCircle )
            {
                if ( !HasCurve() ) return 0;
                static const kiwiOption_t arcOptions[] = {   // KIWI (2026-10-31): the curves' patches
                    { "Density", KOPT_INT, nullptr, 0, 0, 1, 4, -1 },
                };
                *out = arcOptions; return 1;
            }
            static kiwiOption_t options[] = {
                { "Density", KOPT_INT, nullptr, 0, 0, 1, 4, -1 },
                { "Backside faces", KOPT_TOGGLE, nullptr, 0, 0, 0, 1, -1 },
                { "Wall thickness (inward)", KOPT_NUMFIELD, nullptr, 0, 2, 0, 64, -1 },
            };
            options[2].hi = Units_ToDisplay( MinCircleRadius() * 0.99f );
            *out = options; return 3;
        }
        int OptionValue( int opt ) const override
        { return opt == 0 ? m_density : opt == 1 ? (int)m_backfaces : 0; }
        void OptionChanged( int opt, int value ) override
        {
            if ( opt == 0 ) { KiwiNum_ClearField( 1 ); SetCircleCount( value ); }
            if ( opt == 1 ) { m_backfaces = value != 0; KiwiPrim_SetPatchBackfaces( m_backfaces ); }
            Recompute(); g_nUpdateBits |= 1;
        }

        int HudPrompts( const kiwiPrompt_t **out ) const override
        {
            static const kiwiPrompt_t brush[] = { { "P", "Circle: patch" }, { "[ ]", "Sides" } };
            static const kiwiPrompt_t patch[] = { { "P", "Circle: brush" }, { "[ ]", "Density (1-4)" } };
            static const kiwiPrompt_t arc[]   = { { "P", "Curves: patches" }, { "[ ]", "Arc sides (per full circle)" } };
            static const kiwiPrompt_t arcP[]  = { { "P", "Curves: brush sides" }, { "[ ]", "Density (1-4)" } };
            if ( !m_hasCircle && HasCurve() )
            {
                *out = m_patchMode ? arcP : arc;
                return ( m_patchMode || m_hasArc ) ? 2 : 1;   // a spline has no sides count
            }
            *out = m_patchMode ? patch : brush;
            return m_hasCircle ? 2 : 0;
        }

        bool KeyDown( int vk, unsigned int mods ) override
        {
            if ( !( m_hasCircle || HasCurve() ) || mods ) return false;
            if ( ( vk == 0xDB || vk == 0xDD ) && !PatchCount() && !m_hasCircle && !m_hasArc )
                return false;                    // brush splines: no count to step
            if ( vk == 'P' )
            {
                m_patchMode = !m_patchMode;
                KiwiPrim_SetPatchMode( m_patchMode );
                KiwiNum_ClearField( 1 );
                int count;
                const kiwiNumField_t *fields = RoundFields( &count );
                KiwiNum_UpdateFields( fields, count );
                Recompute();
            }
            else if ( vk == 0xDB || vk == 0xDD )
            {
                KiwiNum_ClearField( 1 );
                SetCircleCount( ( PatchCount() ? m_density : m_sides ) + ( vk == 0xDD ? 1 : -1 ) );
            }
            else return false;
            UpdateHud();
            g_nUpdateBits |= 1;
            return true;
        }

        void NumericFieldChanged( int field, bool has, float world ) override
        {
            if ( field == 2 && m_patchMode && m_hasCircle )
            {
                if ( has && world >= 0 && world <= 65536 )
                { m_thickness = world; KiwiPrim_SetPatchThickness( world ); }
                Recompute(); g_nUpdateBits |= 1; return;
            }
            if ( field == 1 && ( m_hasCircle || m_hasArc ) )
            {
                if ( has ) SetCircleCount( (int)floorf( Units_ToDisplay( world ) + 0.5f ) );
                UpdateHud();
                g_nUpdateBits |= 1;
                return;
            }
            KiwiEditorCommand::NumericFieldChanged( field, has, world );
        }

        bool NumericFieldValue( int field, float *out ) const override
        {
            if ( field == 2 && out && m_patchMode && m_hasCircle ) { *out = m_thickness; return true; }
            if ( field == 1 && out && ( m_hasCircle || m_hasArc ) )
            {
                *out = (float)( PatchCount() ? m_density : m_sides );
                return true;
            }
            if ( field != 0 || !out )
                return false;
            *out = m_dist;                     // SIGNED: which way the prism grew
            return true;
        }

        bool BubbleAnchor( float *out3 ) const override
        {
            if ( !out3 || m_region < 0 )
                return false;
            // Profile centroid lifted to the moving cap.
            for ( int k = 0; k < 3; ++k )
                out3[k] = m_ref[k] + m_plane.normal[k] * m_dist;
            return true;
        }

        void Rebase() override
        {
            // LatchStart owns the rebase correction; applying it here too would double it.
            LatchStart();
        }

        // Anchor the lollipop on the moving cap and fold direction by distance sign.
        // At zero distance the camera chooses the visible side; geometry convention is unchanged.
        bool LollipopHandle( float outAnchor[3], float outDir[3] ) const override
        {
            if ( m_region < 0 || !outAnchor || !outDir )
                return false;
            for ( int k = 0; k < 3; ++k )
                outAnchor[k] = m_ref[k] + m_plane.normal[k] * m_dist;
            const float side = KiwiLollipop_FaceSide( outAnchor, m_plane.normal, m_dist );
            for ( int k = 0; k < 3; ++k )
                outDir[k] = m_plane.normal[k] * side;
            return true;
        }

        bool Begin() override
        {
            m_region  = -1;
            m_dist    = 0.0f;
            m_hasNum  = false;
            m_invalid = false;
            m_hud[0]  = '\0';
            m_hasCircle = false;
            m_patchMode = KiwiPrim_PatchMode();
            m_density = KiwiPrim_PatchDensity();
            m_backfaces = KiwiPrim_PatchBackfaces();
            m_thickness = KiwiPrim_PatchThickness();
            m_sides = Radiant_ProfileGetInt( "KiwiUX", "CircleExtrudeSides", KPRIM_CYL_SIDES_DEF );
            if ( m_sides < KPRIM_CYL_SIDES_MIN ) m_sides = KPRIM_CYL_SIDES_MIN;
            if ( m_sides > KPRIM_CYL_SIDES_MAX ) m_sides = KPRIM_CYL_SIDES_MAX;

            const std::vector<kregion_t> &regions = KiwiRegion_All();
            if ( regions.empty() )
            {
                Sys_Printf( "Extrude: no closed construction region.\n" );
                // Explain failed loop detection; force output because this refusal is user-visible.
                KiwiRegion_ReportGaps( true );
                return false;
            }

            // A deliberate region selection outranks cursor picking.
            m_region = KiwiRegion_SelectedIndex();
            m_extra.clear();

            ray_t ray;
            if ( m_region < 0 && Pick_RayFromCursor( &ray ) )
                m_region = KiwiRegion_PickAt( ray );
            if ( m_region < 0 && regions.size() == 1 )
                m_region = 0;                     // the documented "only one" shortcut
            if ( m_region < 0 )
            {
                Sys_Printf( "Extrude: put the cursor over a region first "
                            "(%i available).\n", (int)regions.size() );
                return false;
            }

            // Latch all selected regions once. The primary region drives axis, reference,
            // and numeric depth; each source later extrudes along its own normal.
            {
                const int selN = KiwiRegion_SelectedCount();
                for ( int s = 0; s < selN; ++s )
                {
                    const int idx = KiwiRegion_SelectedAt( s );
                    if ( idx >= 0 && idx != m_region )
                        m_extra.push_back( idx );
                }
            }

            std::vector<int> sources;
            EachSource( &sources );
            m_hasArc = false;
            m_hasSpline = false;
            for ( int source : sources )
            {
                if ( CircleSource( regions[source] ) )
                    m_hasCircle = true;
                else if ( regions[source].holes.empty() )
                {
                    // KIWI (2026-10-31): an outline with arc runs gets the "sides" field too, and
                    // one with an outward spline run P (curve patches).
                    std::vector<float> probe = regions[source].pts;
                    if ( ResampleRoundRuns( probe, regions[source].plane, m_sides ) )
                        m_hasArc = true;
                    std::vector<curveRun_t> runs;
                    probe = regions[source].pts;
                    if ( ShapeSplineRuns( probe, regions[source].plane, &runs ) )
                        m_hasSpline = true;
                }
            }
            if ( m_hasCircle || HasCurve() )
            {
                int count;
                const kiwiNumField_t *fields = RoundFields( &count );
                KiwiNum_SetFields( fields, count );
            }

            const kregion_t &reg = regions[m_region];
            if ( (int)( reg.pts.size() / 2 ) > KEXT_MAX_PROFILE )
            {
                Sys_Printf( "Extrude: profile has %i vertices, the v1 limit is %i.\n",
                            (int)( reg.pts.size() / 2 ), KEXT_MAX_PROFILE );
                return false;
            }

            // Plane-space profile centroid lifted onto the region plane.
            m_plane = reg.plane;
            float c[2] = { 0.0f, 0.0f };
            const std::vector<float> &outerLoop = KiwiRegion_OuterLoop( reg );   // a ring's own rim
            const int n = (int)( outerLoop.size() / 2 );
            for ( int i = 0; i < n; ++i )
            {
                c[0] += outerLoop[i * 2 + 0];
                c[1] += outerLoop[i * 2 + 1];
            }
            c[0] /= (float)n;
            c[1] /= (float)n;
            KiwiCon_PlaneToWorld( m_plane, c, m_ref );
            if ( const kconObject_t *circle = CircleSource( reg ) )
                KiwiCon_PlaneToWorld( circle->plane, circle->centre, m_ref );
            m_plane = UprightRegion( reg ).plane;   // KIWI: a ground face extrudes up (m_ref is world)

            m_dist  = 0.0f;
            m_axisBlocked = false;  // recomputed on the first move
            LatchStart();
            UpdateHud();
            // Circle output is chosen at extrusion time; subtraction remains explicit Q.
            Sys_Printf( "Extrude: drag along the region normal, or type a distance.\n" );
            return true;
        }

        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick;
            m_snap = snap;
            Recompute();
            g_nUpdateBits |= 1;
        }

        // Idle preemption is safe only before any distance or numeric input applies.
        // Region extrusion mutates and opens undo only inside commit, so this state is disposable.
        bool PreemptIdle() const override
        {
            return m_region >= 0 && !m_hasNum && !( fabsf( m_dist ) >= KEXT_MIN_DIST );
        }

        // Transform snap context is raw until Ctrl; no per-command override is needed.

        bool IdlePressReselect( int imgX, int imgY, bool shift ) override
        {
            if ( !PreemptIdle() )
                return false;
            KiwiCmd_Cancel();
            KiwiBox_ClickSelectAt( imgX, imgY, shift, false );
            return true;
        }

        void NumericChanged( bool has, float world ) override
        {
            m_hasNum   = has && KiwiNum_HasValue();
            m_numWorld = world;
            Recompute();
        }

        void Commit() override
        {
            if ( fabsf( m_dist ) < KEXT_MIN_DIST )
            {
                Sys_Printf( "Extrude: zero distance — nothing created.\n" );
                return;
            }
            // Region extrusion deliberately never auto-carves overlapping solids; boolean
            // subtraction is explicit Q. Negative face extrusion below is a separate one-face push.
            Extrude();
            // Clear source-region selection; newly landed brushes stay selected and source lines remain.
            KiwiRegion_ClearSelection();
        }

        void Cancel() override
        {
            m_region = -1;
            m_extra.clear();            // selected sources are per gesture
            g_nUpdateBits |= 1;
        }

        // Three wire segments per profile vertex per source, plus snap-marker headroom.
        int LineBudget() const override
        {
            const std::vector<kregion_t> &regions = KiwiRegion_All();
            std::vector<int> srcs;
            EachSource( &srcs );
            int segs = 0;
            for ( size_t i = 0; i < srcs.size(); ++i )
            {
                segs += 3 * (int)( EffectiveRegion( regions[(size_t)srcs[i]] ).pts.size() / 2 );
                if ( CircleSource( regions[(size_t)srcs[i]] ) ) segs += 48;
                if ( m_patchMode && m_thickness > 0 && CircleSource( regions[(size_t)srcs[i]] ) ) segs += 5 * ( m_density+3 ) * 8;
            }
            return segs + 96;          // …plus the snap marker's own headroom
        }

        void DrawWorld() override
        {
            const std::vector<kregion_t> &regions = KiwiRegion_All();
            std::vector<int> srcs;
            EachSource( &srcs );
            for ( size_t si = 0; si < srcs.size(); ++si )
            {
            const kregion_t reg = EffectiveRegion( regions[(size_t)srcs[si]] );

            prismBuild_t pb;
            MakeBuildFor( reg, &pb );

            const float *col = m_invalid ? KEXT_COL_BAD : KEXT_COL_PREVIEW;
            if ( const kconObject_t *circle = CircleSource( reg ) )
            {
                float uv[2], bottom[3], top[3];
                CircleCentre( *circle, reg.plane, uv );
                PrismVertex( pb, uv, false, bottom );
                PrismVertex( pb, uv, true, top );
                KiwiPrim_DrawCylinderAxis( bottom, top );
            }

            // Draw translucent solid before wireframe; zero depth is rejected by EmitPrismSolid.
            {
                std::vector<int> capTris;
                if ( !( m_patchMode && CircleSource( reg ) ) )
                    KiwiRegion_Triangulate( reg.pts, &capTris );
                EmitPrismSolid( pb, reg.pts, capTris, col, &reg.bridge );
            }

            KiwiLines_Color( col[0], col[1], col[2] );

            if ( const kconObject_t *circle = CircleSource( reg ) )
            if ( m_patchMode && m_thickness > 0 && m_thickness < circle->radius )
            {
                std::vector<float> inner = reg.pts;
                float uv[2]; CircleCentre( *circle, reg.plane, uv );
                const float scale = (circle->radius-m_thickness)/circle->radius;
                for ( size_t i = 0; i < inner.size(); i += 2 )
                    for ( int k = 0; k < 2; ++k ) inner[i+k] = uv[k]+(inner[i+k]-uv[k])*scale;
                const std::vector<int> noCap;
                EmitPrismSolid( pb, inner, noCap, col );
                EmitPrismWire( pb, inner );
                EmitCylinderRims( pb, reg.pts, inner, col );
            }

            const int n = (int)( reg.pts.size() / 2 );
            for ( int i = 0; i < n; ++i )
            {
                if ( KiwiRegion_EdgeIsBridge( reg, i ) )
                    continue;                      // a ring's slit has no wall (KIWI 2026-09-28)
                const int j = ( i + 1 ) % n;
                float a[3], b[3];
                PrismVertex( pb, &reg.pts[(size_t)i * 2], true, a );
                PrismVertex( pb, &reg.pts[(size_t)j * 2], true, b );
                if ( !KiwiLines_Add( a, b ) )
                    return;
                // Include the source ring so the preview reads as a closed prism.
                PrismVertex( pb, &reg.pts[(size_t)i * 2], false, a );
                PrismVertex( pb, &reg.pts[(size_t)j * 2], false, b );
                if ( !KiwiLines_Add( a, b ) )
                    return;
                PrismVertex( pb, &reg.pts[(size_t)i * 2], false, a );
                PrismVertex( pb, &reg.pts[(size_t)i * 2], true,  b );
                if ( !KiwiLines_Add( a, b ) )
                    return;
            }
            }   // end per-source loop
        }

    private:
        float MinCircleRadius() const
        {
            float radius = 65536;
            std::vector<int> sources; EachSource( &sources );
            const auto &regions = KiwiRegion_All();
            for ( int index : sources )
                if ( const kconObject_t *circle = CircleSource( regions[index] ) )
                    if ( circle->radius < radius ) radius = circle->radius;
            return radius;
        }
        const kconObject_t *CircleSource( const kregion_t &reg ) const
        {
            if ( !reg.holes.empty() )
                return nullptr;             // a ring (circle with a hole): its bridged loop, not a cylinder
            const kconObject_t *o = KiwiCon_At( reg.sourceObject );
            return o && o->type == KCON_CIRCLE && !o->hidden ? o : nullptr;
        }

        // In patch mode circles and outline curves (arcs, splines: curve patches, PatchCurves)
        // count density; out of it, sides.  Arcs bulging inward stay brush sides at the saved
        // count, and splines that cannot take a flat cap their tessellation.
        bool HasCurve() const    { return m_hasArc || m_hasSpline; }
        bool PatchCount() const  { return ( m_hasCircle || HasCurve() ) && m_patchMode; }
        bool PatchCurves() const { return HasCurve() && m_patchMode; }

        // The typed fields with a circle or curves in the sources: a patch circle adds density
        // and wall thickness; curve patches (user: "hide the sides field if P mode is active")
        // take length alone; brush circles / arcs, length and sides.
        const kiwiNumField_t *RoundFields( int *count ) const
        {
            if ( m_patchMode && m_hasCircle )              { *count = 3; return KEXT_PATCH_FIELDS; }
            if ( m_patchMode || !( m_hasCircle || m_hasArc ) ) { *count = 1; return KEXT_FIELDS; }
            *count = 2;
            return KEXT_CIRCLE_FIELDS;
        }

        void SetCircleCount( int count )
        {
            const int lo = PatchCount() ? KPRIM_PATCH_DENSITY_MIN : KPRIM_CYL_SIDES_MIN;
            const int hi = PatchCount() ? KPRIM_PATCH_DENSITY_MAX : KPRIM_CYL_SIDES_MAX;
            if ( count < lo ) count = lo;
            if ( count > hi ) count = hi;
            if ( PatchCount() )
            {
                m_density = count;
                KiwiPrim_SetPatchDensity( count );
            }
            else
            {
                m_sides = count;
                Radiant_ProfileSetInt( "KiwiUX", "CircleExtrudeSides", count );
            }
        }

        void CircleCentre( const kconObject_t &circle, const kconPlane_t &plane, float uv[2] ) const
        {
            float world[3];
            KiwiCon_PlaneToWorld( circle.plane, circle.centre, world );
            KiwiCon_WorldToPlane( plane, world, uv );
        }

        // Sampling belongs to the output, never to the analytic source object.
        // KIWI (2026-10-31, user: "fix this nasty direction mismatch with multiple construction
        // faces"): one signed depth drives every source along its own normal, so a selected face
        // on a parallel plane facing the other way grew the opposite way.  Ground faces turn up,
        // then any source facing against the primary's direction is turned to agree with it.
        kregion_t OrientedRegion( const kregion_t &reg ) const
        {
            const kregion_t up = UprightRegion( reg );
            return ( Dot3( up.plane.normal, m_plane.normal ) < -1.0e-3f ) ? FlipRegion( up ) : up;
        }

        kregion_t EffectiveRegion( const kregion_t &reg ) const
        {
            kregion_t result = OrientedRegion( reg );
            const kconObject_t *circle = CircleSource( reg );
            if ( !circle )
            {
                // KIWI (2026-10-31): arc runs in the outline take the circle's side count - or,
                // the arcs that become curve patches (patch mode), a fine count, so the preview
                // shows the curve itself.
                if ( result.holes.empty() )
                    ShapeRoundRuns( result.pts, result.plane, m_sides, PatchCurves() ? 48 : m_sides, nullptr );
                return result;
            }
            float centre[2];
            CircleCentre( *circle, result.plane, centre );
            result.pts.clear();
            const int samples = 8;
            const int n = m_patchMode ? ( m_density + 3 ) * samples : m_sides;
            for ( int i = 0; i < n; ++i )
            {
                float uv[2];
                if ( m_patchMode )
                {
                    float c[3][2];
                    const int span = i / samples;
                    const float t = (float)( i % samples ) / samples, u = 1.0f - t;
                    for ( int j = 0; j < 3; ++j )
                        KiwiPrim_PatchControlPoint( circle->radius, m_density, span * 2 + j, c[j] );
                    for ( int k = 0; k < 2; ++k )
                        uv[k] = u*u*c[0][k] + 2*u*t*c[1][k] + t*t*c[2][k];
                }
                else
                {
                    const float angle = (float)( 2.0 * KIWI_PI ) * i / n;
                    uv[0] = circle->radius * cosf( angle );
                    uv[1] = circle->radius * sinf( angle );
                }
                result.pts.push_back( centre[0] + uv[0] );
                result.pts.push_back( centre[1] + uv[1] );
            }
            return result;
        }

        // Every consumer builds from its current source plane; sharing the primary plane
        // would push sources on other walls sideways.

        // One gesture applies one signed distance to every source, but each source keeps
        // its own plane and normal. The primary source drives cursor and numeric mapping.
        void MakeBuildFor( const kregion_t &reg, prismBuild_t *pb ) const
        {
            pb->plane = reg.plane;
            pb->lo    = ( m_dist >= 0.0f ) ? 0.0f  : m_dist;
            pb->hi    = ( m_dist >= 0.0f ) ? m_dist : 0.0f;
        }

        // Emit the primary first and bounds-check latched indices against the current region list.
        void EachSource( std::vector<int> *out ) const
        {
            out->clear();
            const std::vector<kregion_t> &regions = KiwiRegion_All();
            if ( m_region >= 0 && m_region < (int)regions.size() )
                out->push_back( m_region );
            for ( size_t i = 0; i < m_extra.size(); ++i )
            {
                const int idx = m_extra[i];
                if ( idx < 0 || idx >= (int)regions.size() || idx == m_region )
                    continue;
                out->push_back( idx );
            }
        }

        // Append one region's unlinked, validated defs. On failure the caller frees
        // the complete accumulated list so a multi-source gesture remains all-or-nothing.
        bool BuildRegionDefs( const kregion_t &source, std::vector<brush_t *> *out,
                              const char **why ) const
        {
            const kconObject_t *circle = CircleSource( source );
            if ( circle && m_patchMode )
            {
                const kconPlane_t plane = OrientedRegion( source ).plane;   // KIWI: one direction for all
                float centre[2];
                CircleCentre( *circle, plane, centre );
                return KiwiPrim_BuildPatchCylinderShell( plane, centre, circle->radius,
                    m_dist < 0.0f ? m_dist : 0.0f, m_dist > 0.0f ? m_dist : 0.0f,
                    m_density, m_backfaces, m_thickness, out, why );
            }
            // KIWI (2026-10-31): patch mode on an outline with arcs / splines - each one bulging
            // out becomes curve patches (wall + caps), and the brushes take its chord instead.
            std::vector<curveRun_t> curveRuns;
            kregion_t reg;
            if ( !circle && PatchCurves() && source.holes.empty() )
            {
                reg = OrientedRegion( source );
                ShapeSplineRuns( reg.pts, reg.plane, &curveRuns );
                ShapeRoundRuns( reg.pts, reg.plane, m_sides, m_sides, &curveRuns );
            }
            else
                reg = EffectiveRegion( source );
            prismBuild_t pb;
            MakeBuildFor( reg, &pb );
            for ( size_t i = 0; i < curveRuns.size(); ++i )
                if ( !KiwiPrim_BuildCurvePatches( reg.plane, curveRuns[i].ctrl.data(), (int)( curveRuns[i].ctrl.size() / 2 ),
                                                  pb.lo, pb.hi, m_density, out, why ) )
                    return false;
            const int n = (int)( reg.pts.size() / 2 );
            if ( n < 3 && !curveRuns.empty() )
                return true;                          // curves only (a lens): no brush between them
            if ( n < 3 || n > KEXT_MAX_PROFILE )
            {
                *why = "profile out of range";
                return false;
            }

            std::vector< std::vector<int> > pieces;
            if ( KiwiRegion_IsConvex( reg.pts ) )
            {
                std::vector<int> whole;
                for ( int i = 0; i < n; ++i )
                    whole.push_back( i );
                pieces.push_back( whole );
            }
            else if ( !KiwiRegion_ConvexPieces( reg.pts, &pieces ) )
            {
                *why = "could not decompose the profile (self-intersecting or degenerate)";
                return false;
            }
            // KIWI (2026-10-31, user: cod4map "pointCount <= 64" assert): a CoD4 brush face holds
            // at most 64 points (WindingFromPoints, cod4map common/polylib.cpp), and a piece's caps
            // carry all its corners.  A convex piece with more is cut along a diagonal - both halves
            // stay convex - until every piece fits.
            for ( size_t p = 0; p < pieces.size(); ++p )
            {
                if ( (int)pieces[p].size() <= KEXT_MAX_FACE_POINTS )
                    continue;
                std::vector<int> rest;
                rest.push_back( pieces[p][0] );
                rest.insert( rest.end(), pieces[p].begin() + ( KEXT_MAX_FACE_POINTS - 1 ), pieces[p].end() );
                pieces[p].resize( KEXT_MAX_FACE_POINTS );
                pieces.push_back( rest );             // checked again further down this loop
            }
            if ( pieces.empty() || (int)pieces.size() > KEXT_MAX_PIECES )
            {
                *why = "the decomposition produced too many pieces";
                return false;
            }

            for ( size_t p = 0; p < pieces.size(); ++p )
            {
                brush_t *def = BuildPieceDef( pb, reg.pts, pieces[p], why );
                if ( !def )
                    return false;
                out->push_back( def );
                KiwiValid_Rebuild( def );
                if ( !KiwiValid_CheckBrush( def, why ) )
                    return false;
            }
            return true;
        }

        // A world-aligned end reports its absolute axis coordinate; a slanted end
        // reports signed distance from the source plane to avoid naming a false axis.
        void AbsoluteLabel( char *buf, int bufSize ) const
        {
            int worldAxis = -1;
            for ( int k = 0; k < 3; ++k )
                if ( fabsf( m_plane.normal[k] ) > 0.999f )
                    worldAxis = k;
            char v[32];
            if ( worldAxis >= 0 )
            {
                const float pos = m_ref[worldAxis] + m_plane.normal[worldAxis] * m_dist;
                KiwiUnits_Format( v, sizeof( v ), pos );
                _snprintf( buf, bufSize, "%c %s", "XYZ"[worldAxis], v );
            }
            else
            {
                KiwiUnits_Format( v, sizeof( v ), m_dist );
                _snprintf( buf, bufSize, "out %s", v );
            }
            buf[bufSize - 1] = '\0';
        }

        bool CursorRay( ray_t *out ) const
        {
            int x, y;
            if ( !KiwiCmd_LastCursor( &x, &y ) )
                return false;
            return Pick_RayFromImagePos( x, y, out );
        }

        // Bias the latched cursor coordinate by current distance so relatching is a rebase.
        void LatchStart()
        {
            m_haveStart = false;
            if ( !KiwiCam_AxisPortrayable( m_plane.normal ) )
                return;
            ray_t ray;
            float p[3];
            if ( !CursorRay( &ray ) || !KiwiCam_RayAxis( ray, m_ref, m_plane.normal, p ) )
                return;
            float rel[3];
            Sub3( p, m_ref, rel );
            m_start     = Dot3( rel, m_plane.normal ) - m_dist;
            m_haveStart = true;
        }

        void Recompute()
        {
            float d = m_dist;
            m_gridMajor = false;               // one frame's answer

            // Hold depth while the camera cannot portray this axis, then rebase when it can.
            // Numeric input and snap resolution do not depend on the cursor-axis solve.
            const bool canAxis = KiwiCam_AxisPortrayable( m_plane.normal );
            if ( !canAxis )
                m_haveStart = false;
            else if ( !m_haveStart )
                LatchStart();
            m_axisBlocked = !canAxis;

            // Ctrl-up rebases relative mode at the current depth; Ctrl-down deliberately
            // switches directly to absolute cursor depth.
            const bool absNow = KiwiExt_AbsoluteHeld();
            if ( !absNow && m_absPrev )
                LatchStart();
            m_absPrev  = absNow;
            m_absolute = absNow && canAxis && m_haveStart;

            ray_t ray;
            float p[3];
            float rawAbs  = d;                 // the cursor's own axis position
            bool  haveRaw = false;
            if ( canAxis && m_haveStart && CursorRay( &ray )
              && KiwiCam_RayAxis( ray, m_ref, m_plane.normal, p ) )
            {
                float rel[3];
                Sub3( p, m_ref, rel );
                rawAbs  = Dot3( rel, m_plane.normal );
                haveRaw = true;
                d = absNow ? rawAbs : ( rawAbs - m_start );
            }

            if ( m_hasNum )
            {
                d = m_numWorld;
            }
            else if ( m_absolute && haveRaw )
            {
                // Shared absolute ladder: geometry, face magnet, then major-aware lattice.
                d = KiwiExt_LadderDepth( m_snap, m_ref, m_plane.normal, rawAbs,
                                         &m_gridMajor );
            }
            else if ( m_snap.valid && m_snap.type != SNAP_NONE )
            {
                if ( KiwiSnap_IsGeometry( m_snap.type ) )
                {
                    // Resolve face snaps as planes on the gesture axis so every point on
                    // one face yields one depth; edge-on faces provide no answer.
                    float sd = 0.0f;
                    if ( KiwiSnap_AxisDepth( m_snap, m_ref, m_plane.normal, &sd ) )
                    {
                        // Refuse source-plane and near-zero geometry snaps.
                        if ( fabsf( sd ) >= KEXT_SELF_SNAP_BAND )
                            d = sd;
                    }
                }
                else
                {
                    // Quantize the scalar depth, not the cursor point. The hard lattice steps
                    // whole grid cells and can prefer nearby major lines; this arm
                    // is reached only when the transform snap context is engaged.
                    bool major = false;
                    d = KiwiSnap_LatticeAxis( d, m_ref, m_plane.normal, true, &major );
                    m_gridMajor = major;
                }
            }
            // Without an engaged snap answer, preserve the raw mapped depth.

            m_dist    = d;
            m_invalid = ( fabsf( d ) < KEXT_MIN_DIST );
            if ( m_hasCircle && m_patchMode && m_thickness >= MinCircleRadius() ) m_invalid = true;
            UpdateHud();
        }

        void UpdateHud()
        {
            char b[32];
            KiwiUnits_Format( b, sizeof( b ), m_dist );
            if ( m_hasCircle )
            {
                if ( m_patchMode && m_thickness >= MinCircleRadius() )
                {
                    snprintf( m_hud, sizeof(m_hud), "Circle: wall thickness must be smaller than the smallest circle radius" );
                    return;
                }
                const float nz = fabsf( m_plane.normal[2] );
                const float tilt = acosf( nz > 1.0f ? 1.0f : nz ) * KCON_RAD2DEG;
                const char *status = m_axisBlocked && !m_hasNum ? "ORBIT or type length" :
                                     m_invalid ? "TOO THIN" : "Enter: commit";
                _snprintf( m_hud, sizeof( m_hud ),
                    "extrude circle %s  %s %i  length %s  ·  axis %.1f deg from vertical  ·  P: patch/brush  ·  %s%s",
                    m_patchMode ? "PATCH" : "BRUSH", m_patchMode ? "density" : "sides",
                    m_patchMode ? m_density : m_sides, b, tilt, status,
                    m_patchMode ? (m_thickness > 0 ? "  ·  open bore, no collision" : "  ·  no caps/collision") : "" );
                m_hud[sizeof( m_hud ) - 1] = '\0';
                return;
            }
            const std::vector<kregion_t> &regions = KiwiRegion_All();
            const int verts = ( m_region >= 0 && m_region < (int)regions.size() )
                            ? (int)( EffectiveRegion( regions[m_region] ).pts.size() / 2 ) : 0;
            // Show source count because some selected regions may be off screen.
            char many[48];
            many[0] = '\0';
            if ( !m_extra.empty() )
                _snprintf( many, sizeof( many ), " x%i", (int)m_extra.size() + 1 );
            many[sizeof( many ) - 1] = '\0';
            if ( HasCurve() )          // KIWI (2026-10-31): the curves' count, [ ] / Tab to change
            {
                const size_t len = strlen( many );
                if ( m_patchMode )
                    _snprintf( many + len, sizeof( many ) - len, "  curves as patches, density %i", m_density );
                else if ( m_hasArc )
                    _snprintf( many + len, sizeof( many ) - len, "  arc sides %i/circle", m_sides );
                many[sizeof( many ) - 1] = '\0';
            }

            // The view-gate remedy takes precedence over the derivative TOO THIN state.
            if ( m_axisBlocked && !m_hasNum )
                _snprintf( m_hud, sizeof( m_hud ),
                           "extrude  %i-gon%s  %s  ·  ORBIT to pull — this view looks "
                           "straight along the extrude axis  ·  or type a distance",
                           verts, many, b );
            else if ( m_invalid )
                _snprintf( m_hud, sizeof( m_hud ), "extrude  %i-gon%s  %s  TOO THIN",
                           verts, many, b );
            // Report major locks, and in absolute mode show the aimed coordinate before
            // the familiar signed delta.
            else if ( m_absolute )
            {
                char abso[32];
                AbsoluteLabel( abso, sizeof( abso ) );
                _snprintf( m_hud, sizeof( m_hud ), "extrude  %i-gon%s  %s  (%s)%s  CTRL",
                           verts, many, abso, b, m_gridMajor ? "  [MAJOR]" : "" );
            }
            else
                _snprintf( m_hud, sizeof( m_hud ), "extrude  %i-gon%s  %s%s",
                           verts, many, b, m_gridMajor ? "  [MAJOR]" : "" );
            m_hud[sizeof( m_hud ) - 1] = '\0';
        }


        // Commit.
        void Extrude()
        {
            const std::vector<kregion_t> &regions = KiwiRegion_All();
            std::vector<int> srcs;
            EachSource( &srcs );
            if ( srcs.empty() )
                return;

            // Build and validate every def before linking any. Rejection lands no brush
            // and opens no undo bracket; multiple sources commit all-or-nothing.
            std::vector<brush_t *> defs;
            const char *why = "unknown";
            bool  ok    = true;
            for ( size_t s = 0; s < srcs.size() && ok; ++s )
            {
                const kregion_t &reg = regions[(size_t)srcs[s]];
                ok = BuildRegionDefs( reg, &defs, &why );
            }

            if ( !ok )
            {
                // Unlinked defs satisfy Brush_Free_R's refCount/owner-chain precondition.
                for ( size_t i = 0; i < defs.size(); ++i )
                    Brush_Free_R( defs[i] );
                Sys_Printf( "Extrude: rejected — %s.\n", why ? why : "invalid geometry" );
                return;
            }

            // Deselect before opening one undo bracket so only the new selected brushes
            // are stamped as creations.
            Select_Deselect( 1 );
            KiwiCmd_UndoBegin( "extrude region" );
            for ( size_t i = 0; i < defs.size(); ++i )
                LandDef( defs[i] );

            int patches = 0;
            for ( brush_t *def : defs ) if ( def->patch ) ++patches;
            Sys_Printf( "Extruded %i region(s): %i brush(es), %i patch(es).%s\n",
                        (int)srcs.size(), (int)defs.size() - patches, patches,
                        patches ? " Patches have no caps/collision; add caulk for collision." : "" );
            g_nUpdateBits = -1;
        }

        int              m_region  = -1;
        bool             m_hasCircle = false;
        bool             m_hasArc = false;      // a non-circle source's outline has arc runs
        bool             m_hasSpline = false;   // ...or an outward spline run (P: curve patches)
        bool             m_patchMode = false;
        int              m_sides = KPRIM_CYL_SIDES_DEF;
        int              m_density = KPRIM_PATCH_DENSITY_DEF;
        bool             m_backfaces = false;
        float            m_thickness = 0.0f;
        // Other selected regions latched for this gesture; empty is single-source.
        std::vector<int> m_extra;
        kconPlane_t   m_plane;
        float         m_ref[3]  = { 0.0f, 0.0f, 0.0f };
        float         m_start   = 0.0f;
        bool          m_haveStart = false;
        float         m_dist    = 0.0f;
        bool          m_axisBlocked = false;  // view gate
        bool          m_hasNum  = false;
        float         m_numWorld = 0.0f;
        bool          m_invalid = false;
        // True only for the current frame when a major grid line supplied the depth.
        bool          m_gridMajor = false;
        // Ctrl edge detector and current absolute-mode state.
        bool          m_absPrev   = false;
        bool          m_absolute  = false;
        snap_result_t m_snap;
        char          m_hud[192] = { 0 };
    };

    // Face extrusion grows a separate prism and leaves the source brush untouched.
    // Project the convex face winding into a right-handed plane basis and normalize it
    // to CCW before using the shared prism writer. Negative depth delegates to the
    // existing one-face push/destroy path instead of building an inward body.
    class KiwiExtrudeFaceCommand : public KiwiEditorCommand
    {
    public:
        const char *Name() const override { return "Extrude Face"; }
        // Begin performs the refusal so it can print a useful message.
        bool CanExecute() override { return true; }

        const char *HudStatus() const override { return m_hud[0] ? m_hud : 0; }
        bool        HudInvalid() const override { return m_invalid; }

        int NumericFields( const kiwiNumField_t **out ) const override
        { *out = KEXT_FIELDS; return 1; }

        bool NumericFieldValue( int field, float *out ) const override
        {
            if ( field != 0 || !out )
                return false;
            *out = m_dist;
            return true;
        }

        bool BubbleAnchor( float *out3 ) const override
        {
            if ( !out3 || !m_have )
                return false;
            for ( int k = 0; k < 3; ++k )
                out3[k] = m_ref[k] + m_plane.normal[k] * m_dist;
            return true;
        }

        // Face extrusion is a transform: raw depth until Ctrl engages snapping.

        // Three wire segments per source vertex, plus snap-marker headroom.
        int LineBudget() const override
        {
            int segs = 3 * (int)( m_loop.size() / 2 );
            for ( size_t i = 0; i < m_extraFaces.size(); ++i )
                segs += 3 * (int)( m_extraFaces[i].loop.size() / 2 );
            return segs + 96;
        }

        // Never pick or snap the selected geometry this transform grows from.
        unsigned PickFlags() const override { return PICKF_EXCLUDE_SELECTED; }

        void Rebase() override
        {
            // LatchStart owns distance-preserving rebase.
            LatchStart();
        }

        // Keep the lollipop on the moving face and fold direction by depth sign.
        // At rest the camera chooses the visible side after orbiting.
        bool LollipopHandle( float outAnchor[3], float outDir[3] ) const override
        {
            if ( !m_have || !outAnchor || !outDir )
                return false;
            for ( int k = 0; k < 3; ++k )
                outAnchor[k] = m_ref[k] + m_plane.normal[k] * m_dist;
            const float side = KiwiLollipop_FaceSide( outAnchor, m_plane.normal, m_dist );
            for ( int k = 0; k < 3; ++k )
                outDir[k] = m_plane.normal[k] * side;
            return true;
        }

        bool Begin() override
        {
            m_have      = false;
            m_dist      = 0.0f;
            m_axisBlocked = false;      // view gate
            m_hasNum    = false;
            m_invalid   = true;
            m_mode      = KEXTF_IDLE;
            m_node      = 0;
            m_faceIndex = -1;
            m_depth     = 0.0f;
            m_haveDepth = false;
            m_hud[0]    = '\0';
            m_loop.clear();
            m_extraFaces.clear();       // selected sources are per gesture

            selbrush_t *node = 0;
            int         face = -1;
            if ( !KiwiExtrudeFace_Pick( &node, &face ) )
            {
                Sys_Printf( "Extrude: select (or hover) a brush face, or put the "
                            "cursor over a construction region.\n" );
                return false;
            }
            if ( !BuildProfile( node, face ) )
                return false;

            // Latch the source face and its inward destroy depth from untouched geometry.
            m_node      = node;
            m_faceIndex = face;
            m_haveDepth = ( KiwiXform_FacePushDepth( node, face, &m_depth ) != 0 );

            // Profile every other selected face once, preserving its own plane.
            // The active face drives the gesture. Extras that cannot form a convex profile
            // are omitted and reported by count instead of rejecting the drive face.
            {
                const selection_t &sel = KiwiSel();
                int dropped = 0;
                for ( size_t i = 0; i < sel.items.size(); ++i )
                {
                    const sel_item_t &it = sel.items[i];
                    // Exclude only the drive face; another face on the same brush is a valid source.
                    if ( it.kind != SEL_FACE || !it.brush )
                        continue;
                    if ( it.brush == m_node && it.faceIndex == m_faceIndex )
                        continue;
                    if ( !Sel_BrushLive( it.brush ) || it.brush->patch )
                        continue;
                    const brush_t *d = it.brush->def;
                    if ( !d || !d->faces || it.faceIndex < 0 || it.faceIndex >= d->faceCount )
                        continue;

                    faceSrc_t fs;
                    fs.node = it.brush;
                    fs.face = it.faceIndex;
                    if ( !BuildProfileInto( it.brush, it.faceIndex, false,
                                            &fs.plane, &fs.loop, 0 ) )
                    {
                        ++dropped;
                        continue;
                    }
                    m_extraFaces.push_back( fs );
                }
                if ( dropped > 0 )
                    Sys_Printf( "Extrude Face: %i selected face(s) could not be "
                                "profiled and are not in this extrusion.\n", dropped );
            }

            m_have = true;
            LatchStart();
            UpdateHud();
            Sys_Printf( "Extrude Face: drag OUT along the face normal for a new "
                        "body, or IN to carve this brush (all the way through "
                        "destroys it).  RMB / Enter confirms.\n" );
            return true;
        }

        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick;
            m_snap = snap;
            Recompute();
            g_nUpdateBits |= 1;
        }

        void NumericChanged( bool has, float world ) override
        {
            m_hasNum   = has && KiwiNum_HasValue();
            m_numWorld = world;
            Recompute();
        }

        void Commit() override
        {
            if ( !m_have || m_mode == KEXTF_IDLE )
            {
                Sys_Printf( "Extrude Face: zero distance — nothing created.\n" );
                return;
            }
            if ( m_mode == KEXTF_GROW )
            {
                Grow();
                return;
            }
            // Negative depth stays single-source: each brush has an independent destroy
            // threshold, so mixing several carve/delete outcomes would be ambiguous.
            if ( !m_extraFaces.empty() )
                Sys_Printf( "Extrude Face: carving IN applies to the ACTIVE face only "
                            "— the other %i selected face(s) are left alone.  Drag OUT "
                            "to grow all of them at once.\n", (int)m_extraFaces.size() );
            Carve();
        }

        void Cancel() override
        {
            m_have = false;
            m_extraFaces.clear();       // selected sources are per gesture
            g_nUpdateBits |= 1;
        }

        void DrawWorld() override
        {
            if ( !m_have )
                return;

            // The gesture mutates nothing before commit. Preview carve at the translated
            // source winding, or outline the whole doomed brush in delete red.
            if ( m_mode == KEXTF_CARVE || m_mode == KEXTF_DESTROY )
            {
                DrawCarve();
                return;
            }

            // Draw extras first so the drive-face preview remains on top.
            for ( size_t i = 0; i < m_extraFaces.size(); ++i )
            {
                const faceSrc_t &fs = m_extraFaces[i];
                if ( fs.loop.size() < 6 )
                    continue;
                prismBuild_t xb;
                xb.plane = fs.plane;
                xb.lo    = 0.0f;
                xb.hi    = ( m_dist > 0.0f ) ? m_dist : 0.0f;
                const float *xc = m_invalid ? KEXT_COL_BAD : KEXT_COL_PREVIEW;
                std::vector<int> xtris;
                KiwiRegion_Triangulate( fs.loop, &xtris );
                EmitPrismSolid( xb, fs.loop, xtris, xc );
                KiwiLines_Color( xc[0], xc[1], xc[2] );
                EmitPrismWire( xb, fs.loop );
            }

            prismBuild_t pb;
            pb.plane = m_plane;
            pb.lo    = 0.0f;
            pb.hi    = ( m_dist > 0.0f ) ? m_dist : 0.0f;

            const float *col = m_invalid ? KEXT_COL_BAD : KEXT_COL_PREVIEW;
            KiwiLines_Color( col[0], col[1], col[2] );

            // Suppress sub-threshold previews; a collapsed prism would only retrace the source face.
            if ( fabsf( m_dist ) < KEXT_MIN_DIST )
                return;

            // Draw the translucent solid before its wireframe.
            {
                std::vector<int> capTris;
                KiwiRegion_Triangulate( m_loop, &capTris );
                EmitPrismSolid( pb, m_loop, capTris, col );
                KiwiLines_Color( col[0], col[1], col[2] );
            }

            const int n = (int)( m_loop.size() / 2 );
            for ( int i = 0; i < n; ++i )
            {
                const int j = ( i + 1 ) % n;
                float a[3], b[3];
                PrismVertex( pb, &m_loop[(size_t)i * 2], true, a );
                PrismVertex( pb, &m_loop[(size_t)j * 2], true, b );
                if ( !KiwiLines_Add( a, b ) )
                    return;
                // Source ring.
                PrismVertex( pb, &m_loop[(size_t)i * 2], false, a );
                PrismVertex( pb, &m_loop[(size_t)j * 2], false, b );
                if ( !KiwiLines_Add( a, b ) )
                    return;
                PrismVertex( pb, &m_loop[(size_t)i * 2], false, a );
                PrismVertex( pb, &m_loop[(size_t)i * 2], true,  b );
                if ( !KiwiLines_Add( a, b ) )
                    return;
            }
        }

    private:
        // Carve previews the translated source winding; destroy outlines every brush winding.
        void DrawCarve()
        {
            if ( !m_node || !Sel_BrushLive( m_node ) )
                return;
            const brush_t *def = m_node->def;
            if ( !def || !def->faces )
                return;

            if ( m_mode == KEXTF_DESTROY )
            {
                KiwiLines_Color( KEXT_COL_DOOMED[0], KEXT_COL_DOOMED[1],
                                 KEXT_COL_DOOMED[2] );
                for ( int f = 0; f < def->faceCount; ++f )
                {
                    const winding_t *w = def->faces[f].w;
                    if ( !w || w->numpoints < 2 || w->numpoints > MAX_POINTS_ON_WINDING )
                        continue;
                    for ( int p = 0; p < w->numpoints; ++p )
                        if ( !KiwiLines_Add( w->p[p], w->p[( p + 1 ) % w->numpoints] ) )
                            return;
                }
                return;
            }

            if ( m_faceIndex < 0 || m_faceIndex >= def->faceCount )
                return;
            const winding_t *w = def->faces[m_faceIndex].w;
            if ( !w || w->numpoints < 2 || w->numpoints > MAX_POINTS_ON_WINDING )
                return;

            KiwiLines_Color( KEXT_COL_CARVE[0], KEXT_COL_CARVE[1], KEXT_COL_CARVE[2] );
            for ( int p = 0; p < w->numpoints; ++p )
            {
                const int q = ( p + 1 ) % w->numpoints;
                float a[3], b[3];
                for ( int k = 0; k < 3; ++k )
                {
                    a[k] = w->p[p][k] + m_plane.normal[k] * m_dist;   // m_dist < 0
                    b[k] = w->p[q][k] + m_plane.normal[k] * m_dist;
                }
                if ( !KiwiLines_Add( a, b ) )
                    return;
            }
        }

        // Delegate negative depth to the interactive push helper so deletion threshold,
        // texture locking, validity, and removal ordering stay in one implementation.
        // Its undo bracket remains open for the framework to commit; rejection cancels it.
        void Carve()
        {
            const char *why = 0;
            const int r = KiwiXform_PushFaceOnce( m_node, m_faceIndex, m_dist,
                                                  "un-extrude face", &why );
            char b[32];
            KiwiUnits_Format( b, sizeof( b ), -m_dist );

            if ( r == KXPUSH_DELETED )
            {
                Sel_Clear( KiwiSel() );
                Sel_RebuildFromLegacy();
                Sys_Printf( "Un-extrude: pushed through — the brush was destroyed.\n" );
            }
            else if ( r == KXPUSH_PUSHED )
            {
                // Rebuild typed selection after windings are re-solved; face count is unchanged.
                Sel_RebuildFromLegacy();
                Sys_Printf( "Un-extrude: carved %s into the brush.\n", b );
            }
            else
            {
                Sys_Printf( "Un-extrude: rejected — %s.\n", why ? why : "invalid geometry" );
            }
            g_nUpdateBits = -1;
        }

        // Write profile results through parameters so drive and extra faces share one path.
        bool BuildProfileInto( selbrush_t *node, int faceIndex, bool verbose,
                               kconPlane_t *outPlane, std::vector<float> *outLoop,
                               float *outRef ) const
        {
            const brush_t *def = node->def;
            const face_t  *f   = &def->faces[faceIndex];
            const winding_t *w = f->w;
            if ( !w || w->numpoints < 3 || w->numpoints > KEXT_MAX_PROFILE )
            {
                if ( verbose )
                    Sys_Printf( "Extrude Face: that face has %i vertices (the v1 limit "
                                "is %i).\n", w ? w->numpoints : 0, KEXT_MAX_PROFILE );
                return false;
            }

            // Seed the plane basis with the face's longest edge.
            float c[3] = { 0.0f, 0.0f, 0.0f };
            for ( int i = 0; i < w->numpoints; ++i )
                for ( int k = 0; k < 3; ++k )
                    c[k] += w->p[i][k];
            const float inv = 1.0f / (float)w->numpoints;
            for ( int k = 0; k < 3; ++k )
                c[k] *= inv;

            float hint[3] = { 0.0f, 0.0f, 0.0f };
            float bestLen = 0.0f;
            for ( int i = 0; i < w->numpoints; ++i )
            {
                float e[3];
                Sub3( w->p[( i + 1 ) % w->numpoints], w->p[i], e );
                const float l = sqrtf( Dot3( e, e ) );
                if ( l > bestLen ) { bestLen = l; Copy3( e, hint ); }
            }

            if ( !KiwiCon_MakePlane( c, f->plane.normal, ( bestLen > 1.0e-3f ) ? hint : 0,
                                     outPlane ) )
            {
                if ( verbose )
                    Sys_Printf( "Extrude Face: that face has a degenerate plane.\n" );
                return false;
            }
            if ( outRef )
                Copy3( c, outRef );

            std::vector<float> raw;
            raw.resize( (size_t)w->numpoints * 2 );
            for ( int i = 0; i < w->numpoints; ++i )
                KiwiCon_WorldToPlane( *outPlane, w->p[i], &raw[(size_t)i * 2] );

            // KIWI (2026-09-28, user: extruding a small split-brush face "fails ... This is
            // BS"): brush windings (the CM-style gift wrap, split halves) can carry a near-
            // duplicate or a near-collinear vertex.  Its tiny edge became a side plane of
            // arbitrary direction that cut the prism ("face collapsed" / "planes crossed").
            // A brush face is convex by construction, so the profile is its CONVEX HULL:
            // points within KEXT_HULL_WELD merged, points within that of a hull edge dropped.
            // The hull is CCW, so the old signed-area flip and convexity gate are implied.
            std::vector<float> &loop = *outLoop;
            if ( !ConvexHull2D( raw, &loop ) )
            {
                if ( verbose )
                    Sys_Printf( "Extrude Face: that face is degenerate (its corners lie "
                                "on one line).\n" );
                return false;
            }
            // The ported winding build merges corners within 0.1 per axis, so a face this
            // narrow can never become a brush.  Say so (always - it is never obvious on
            // screen) instead of letting it die later as "face collapsed".
            const float width = HullWidth( loop );
            if ( width < KEXT_MIN_FACE_WIDTH )
            {
                char wb[32];
                KiwiUnits_Format( wb, sizeof( wb ), width );
                Sys_Printf( "Extrude Face: that face is a SLIVER only %s wide - too thin to "
                            "become a brush.  (Usually left by an old split next to an "
                            "edge; delete the sliver brush.)\n", wb );
                return false;
            }
            return true;
        }

        // Smallest caliper width of a CCW convex loop: per edge, the farthest point.
        static float HullWidth( const std::vector<float> &loop )
        {
            const int n = (int)( loop.size() / 2 );
            float best = 1.0e30f;
            for ( int i = 0; i < n; ++i )
            {
                const int j = ( i + 1 ) % n;
                const float ex = loop[(size_t)j * 2] - loop[(size_t)i * 2];
                const float ey = loop[(size_t)j * 2 + 1] - loop[(size_t)i * 2 + 1];
                const float len = sqrtf( ex * ex + ey * ey );
                if ( len < 1.0e-6f )
                    continue;
                float reach = 0.0f;
                for ( int k = 0; k < n; ++k )
                {
                    const float px = loop[(size_t)k * 2] - loop[(size_t)i * 2];
                    const float py = loop[(size_t)k * 2 + 1] - loop[(size_t)i * 2 + 1];
                    const float d = fabsf( ex * py - ey * px ) / len;
                    if ( d > reach ) reach = d;
                }
                if ( reach < best ) best = reach;
            }
            return best;
        }

        // Andrew's monotone chain over welded points; CCW, no repeated or collinear points.
        static bool ConvexHull2D( const std::vector<float> &in, std::vector<float> *out )
        {
            const float KEXT_HULL_WELD = 0.05f;          // world units
            struct p2_t { float x, y; };
            std::vector<p2_t> pts;
            for ( size_t i = 0; i + 1 < in.size(); i += 2 )
            {
                const p2_t p = { in[i], in[i + 1] };
                bool dup = false;
                for ( size_t k = 0; k < pts.size() && !dup; ++k )
                    dup = fabsf( pts[k].x - p.x ) <= KEXT_HULL_WELD
                       && fabsf( pts[k].y - p.y ) <= KEXT_HULL_WELD;
                if ( !dup )
                    pts.push_back( p );
            }
            out->clear();
            if ( pts.size() < 3 )
                return false;
            std::sort( pts.begin(), pts.end(), []( const p2_t &a, const p2_t &b )
                       { return a.x < b.x || ( a.x == b.x && a.y < b.y ); } );

            // Keep b only when it stands more than the weld off the line a -> c on the hull
            // side; otherwise it is interior, collinear or a near-duplicate and pops.
            auto keepsTurn = [KEXT_HULL_WELD]( const p2_t &a, const p2_t &b, const p2_t &c )
            {
                const float ex = c.x - a.x, ey = c.y - a.y;
                const float len = sqrtf( ex * ex + ey * ey );
                if ( len < 1.0e-6f )
                    return false;
                const float cross = ( b.x - a.x ) * ey - ( b.y - a.y ) * ex;
                return cross / len > KEXT_HULL_WELD;
            };
            std::vector<p2_t> hull;
            for ( int pass = 0; pass < 2; ++pass )
            {
                const size_t start = hull.size();
                for ( size_t i = 0; i < pts.size(); ++i )
                {
                    const p2_t &c = pass ? pts[pts.size() - 1 - i] : pts[i];
                    while ( hull.size() >= start + 2
                         && !keepsTurn( hull[hull.size() - 2], hull[hull.size() - 1], c ) )
                        hull.pop_back();
                    hull.push_back( c );
                }
                hull.pop_back();                 // the next pass starts from this point
            }
            if ( hull.size() < 3 )
                return false;
            for ( size_t i = 0; i < hull.size(); ++i )
            {
                out->push_back( hull[i].x );
                out->push_back( hull[i].y );
            }
            // The prism wants CCW; flip if the chain came out the other way.
            float area2 = 0.0f;
            const int n = (int)hull.size();
            for ( int i = 0; i < n; ++i )
            {
                const int j = ( i + 1 ) % n;
                area2 += hull[i].x * hull[j].y - hull[j].x * hull[i].y;
            }
            if ( area2 < 0.0f )
                for ( int i = 0; i < n / 2; ++i )
                {
                    std::swap( ( *out )[(size_t)i * 2 + 0], ( *out )[(size_t)( n - 1 - i ) * 2 + 0] );
                    std::swap( ( *out )[(size_t)i * 2 + 1], ( *out )[(size_t)( n - 1 - i ) * 2 + 1] );
                }
            return fabsf( area2 ) * 0.5f >= KVALID_MIN_FACE_AREA;
        }

        // Drive-face wrapper; only the drive face reports detailed profile errors.
        bool BuildProfile( selbrush_t *node, int faceIndex )
        {
            return BuildProfileInto( node, faceIndex, true, &m_plane, &m_loop, m_ref );
        }

        // A world-aligned end reports its absolute axis coordinate; a slanted end
        // reports signed distance from the source plane.
        void AbsoluteLabel( char *buf, int bufSize ) const
        {
            int worldAxis = -1;
            for ( int k = 0; k < 3; ++k )
                if ( fabsf( m_plane.normal[k] ) > 0.999f )
                    worldAxis = k;
            char v[32];
            if ( worldAxis >= 0 )
            {
                const float pos = m_ref[worldAxis] + m_plane.normal[worldAxis] * m_dist;
                KiwiUnits_Format( v, sizeof( v ), pos );
                _snprintf( buf, bufSize, "%c %s", "XYZ"[worldAxis], v );
            }
            else
            {
                KiwiUnits_Format( v, sizeof( v ), m_dist );
                _snprintf( buf, bufSize, "out %s", v );
            }
            buf[bufSize - 1] = '\0';
        }

        bool CursorRay( ray_t *out ) const
        {
            int x, y;
            if ( !KiwiCmd_LastCursor( &x, &y ) )
                return false;
            return Pick_RayFromImagePos( x, y, out );
        }

        // Relatch as a distance-preserving rebase.
        void LatchStart()
        {
            m_haveStart = false;
            if ( !KiwiCam_AxisPortrayable( m_plane.normal ) )
                return;
            ray_t ray;
            float p[3];
            if ( !CursorRay( &ray ) || !KiwiCam_RayAxis( ray, m_ref, m_plane.normal, p ) )
                return;
            float rel[3];
            Sub3( p, m_ref, rel );
            m_start     = Dot3( rel, m_plane.normal ) - m_dist;
            m_haveStart = true;
        }

        void Recompute()
        {
            float d = m_dist;
            m_gridMajor = false;               // one frame's answer

            // Hold and later rebase when the camera looks along the face normal and cannot
            // portray motion on that axis.
            const bool canAxis = KiwiCam_AxisPortrayable( m_plane.normal );
            if ( !canAxis )
                m_haveStart = false;
            else if ( !m_haveStart )
                LatchStart();
            m_axisBlocked = !canAxis;

            // Ctrl-up rebases relative mode at the current depth; Ctrl-down switches
            // directly to absolute cursor depth.
            const bool absNow = KiwiExt_AbsoluteHeld();
            if ( !absNow && m_absPrev )
                LatchStart();
            m_absPrev  = absNow;
            m_absolute = absNow && canAxis && m_haveStart;

            ray_t ray;
            float p[3];
            float rawAbs  = d;                 // the cursor's own axis position
            bool  haveRaw = false;
            if ( canAxis && m_haveStart && CursorRay( &ray )
              && KiwiCam_RayAxis( ray, m_ref, m_plane.normal, p ) )
            {
                float rel[3];
                Sub3( p, m_ref, rel );
                rawAbs  = Dot3( rel, m_plane.normal );
                haveRaw = true;
                d = absNow ? rawAbs : ( rawAbs - m_start );
            }

            if ( m_hasNum )
            {
                d = m_numWorld;
            }
            else if ( m_absolute && haveRaw )
            {
                // Shared absolute ladder: geometry, face magnet, then major-aware lattice.
                d = KiwiExt_LadderDepth( m_snap, m_ref, m_plane.normal, rawAbs,
                                         &m_gridMajor );
            }
            else if ( m_snap.valid && m_snap.type != SNAP_NONE )
            {
                if ( KiwiSnap_IsGeometry( m_snap.type ) )
                {
                    // Resolve planar snaps consistently with the region path.
                    float sd = 0.0f;
                    if ( KiwiSnap_AxisDepth( m_snap, m_ref, m_plane.normal, &sd ) )
                    {
                        // Refuse the source-plane band. Pick flags cover selected brush geometry,
                        // while this scalar rule also covers construction and guide candidates.
                        if ( fabsf( sd ) >= KEXT_SELF_SNAP_BAND )
                            d = sd;
                    }
                }
                else
                {
                    // Hard lattice (whole grid steps of depth) with major-line preference.
                    bool major = false;
                    d = KiwiSnap_LatticeAxis( d, m_ref, m_plane.normal, true, &major );
                    m_gridMajor = major;
                }
            }
            // Without an engaged snap answer, preserve raw mapped depth.

            m_dist = d;

            // Recompute grow, carve, or destroy from signed depth every frame.
            // Use the helper's exported epsilon so HUD and commit agree at the threshold.
            m_mode = KEXTF_IDLE;
            if ( d >= KEXT_MIN_DIST )
                m_mode = KEXTF_GROW;
            else if ( d <= -KEXT_MIN_DIST )
                m_mode = ( m_haveDepth && m_depth > KEXT_PUSH_EPS
                        && d <= -m_depth + KEXT_PUSH_EPS )
                       ? KEXTF_DESTROY : KEXTF_CARVE;

            // Only sub-threshold depth is invalid; negative depth is a valid un-extrude.
            m_invalid = ( m_mode == KEXTF_IDLE );
            UpdateHud();
        }

        void UpdateHud()
        {
            char b[32];
            KiwiUnits_Format( b, sizeof( b ), ( m_dist < 0.0f ) ? -m_dist : m_dist );
            const int verts = (int)( m_loop.size() / 2 );

            // The view gate outranks action modes because it holds depth unchanged.
            if ( m_axisBlocked && !m_hasNum )
            {
                _snprintf( m_hud, sizeof( m_hud ),
                           "extrude face  %i-gon  %s  ·  ORBIT to pull — this view "
                           "looks straight along the face normal  ·  or type a distance",
                           verts, b );
                m_hud[sizeof( m_hud ) - 1] = '\0';
                return;
            }

            // State the exact commit outcome for the current signed depth.
            switch ( m_mode )
            {
            case KEXTF_GROW:
                // Include source count for selected faces that may be off screen.
                if ( !m_extraFaces.empty() )
                    _snprintf( m_hud, sizeof( m_hud ),
                               "extrude face  %i-gon x%i  %s  ->  %i NEW BODIES",
                               verts, (int)m_extraFaces.size() + 1, b,
                               (int)m_extraFaces.size() + 1 );
                else
                    _snprintf( m_hud, sizeof( m_hud ),
                               "extrude face  %i-gon  %s  ->  NEW BODY", verts, b );
                break;
            case KEXTF_CARVE:
                _snprintf( m_hud, sizeof( m_hud ),
                           "un-extrude  %s IN  ->  CARVES the brush  (%s to destroy)",
                           b, m_haveDepth ? "keep pulling" : "no depth" );
                break;
            case KEXTF_DESTROY:
                _snprintf( m_hud, sizeof( m_hud ),
                           "un-extrude  %s IN  ->  PUSHED THROUGH, confirm DESTROYS "
                           "this brush", b );
                break;
            default:
                _snprintf( m_hud, sizeof( m_hud ),
                           "extrude face  %i-gon  %s  (pull OUT for a new body, "
                           "IN to carve)", verts, b );
                break;
            }
            // Append the major-lock badge once because it describes every action mode.
            if ( m_gridMajor )
            {
                const size_t n = strlen( m_hud );
                if ( n + 9 < sizeof( m_hud ) )
                    _snprintf( m_hud + n, sizeof( m_hud ) - n, "  [MAJOR]" );
            }
            // Append the current absolute coordinate once for every action mode.
            if ( m_absolute )
            {
                char abso[32];
                AbsoluteLabel( abso, sizeof( abso ) );
                const size_t n = strlen( m_hud );
                if ( n + 24 < sizeof( m_hud ) )
                    _snprintf( m_hud + n, sizeof( m_hud ) - n, "  CTRL @ %s", abso );
            }
            m_hud[sizeof( m_hud ) - 1] = '\0';
        }

        // KIWI (2026-09-28, user: dual-face E "operation fails" - console "rejected ? planes
        // crossed"): one validated prism per source face.  The winding is retried REVERSED
        // when the first build fails the gate - only the correctly oriented prism can pass,
        // so the retry can never land an inside-out brush - and a face that still fails is
        // reported on its own instead of refusing every other face with it.
        static brush_t *BuildGrowPrism( const kconPlane_t &plane, const std::vector<float> &loop,
                                        float dist, const char **why )
        {
            const int n = (int)( loop.size() / 2 );
            if ( n < 3 )
            {
                *why = "profile out of range";
                return 0;
            }
            const char *first = 0;             // the as-built reason outranks the retry's
            for ( int attempt = 0; attempt < 2; ++attempt )
            {
                if ( attempt == 1 )
                    first = *why;
                std::vector<float> uv = loop;
                if ( attempt == 1 )
                    for ( int i = 0; i < n / 2; ++i )
                    {
                        const int j = n - 1 - i;
                        std::swap( uv[(size_t)i * 2 + 0], uv[(size_t)j * 2 + 0] );
                        std::swap( uv[(size_t)i * 2 + 1], uv[(size_t)j * 2 + 1] );
                    }
                brush_t *def = KiwiExtrude_BuildPrismDef( plane, &uv[0], n, 0.0f, dist, why );
                if ( !def )
                    continue;
                KiwiValid_Rebuild( def );
                if ( KiwiValid_CheckBrush( def, why ) )
                {
                    if ( attempt == 1 )
                        Sys_Printf( "Extrude Face: a face's winding was inverted - built it "
                                    "reversed.\n" );
                    return def;
                }
                Brush_Free_R( def );           // never linked: refCount 0, no owner chain
            }
            if ( first )
                *why = first;
            return 0;
        }

        // KIWI (2026-10-05, user: "it's easy to make geometry that fails the compiler"): seat
        // the new body on the SOURCE brush's own plane points.  The frame's origin is the
        // winding centroid and winding points sit up to the CM snap off their planes, so the
        // base was ~0.005 off its source face, and a side lying in a source face was ~1e-6
        // rad off it.  cod4map merges planes only within 1e-7 (normal) / 0.001 (dist)
        // (FindFloatPlane), so both became distinct near-coplanar planes: slivers and
        // fragile corners.  The base is now the source face reversed, the top the same
        // points + dist * n, and a side in the plane of a source face takes its points.
        static void SeatCapsOnSource( brush_t *def, const selbrush_t *node, int faceIndex, float dist )
        {
            if ( !def || def->faceCount < 2 || !node || !node->def
              || faceIndex < 0 || faceIndex >= node->def->faceCount )
                return;
            const brush_t *srcDef = node->def;
            const face_t  *src    = &srcDef->faces[faceIndex];
            const float   *n      = src->plane.normal;
            std::vector<float> saved( (size_t)def->faceCount * 9 );
            for ( int c = 0; c < def->faceCount; ++c )
                memcpy( &saved[(size_t)c * 9], def->faces[c].planepts, sizeof( float ) * 9 );
            for ( int c = 0; c < def->faceCount; ++c )
            {
                face_t *f = &def->faces[c];
                const float d = Dot3( f->plane.normal, n );
                // The caps, by normal: the body spans offsets 0..dist along n, so the cap facing
                // +n sits at max(0, dist), the one facing -n at min(0, dist) (an inward E puts
                // the source plane on the +n cap).  Offset 0 copies the source points exactly.
                if ( fabsf( d ) > 0.999f )
                {
                    const bool  up     = d > 0.0f;
                    const float offset = up ? ( dist > 0.0f ? dist : 0.0f ) : ( dist < 0.0f ? dist : 0.0f );
                    for ( int k = 0; k < 3; ++k )
                    {
                        const float *p = src->planepts[up ? k : 2 - k];   // -n: reversed winding
                        for ( int a = 0; a < 3; ++a )
                            f->planepts[k][a] = offset != 0.0f ? p[a] + n[a] * offset : p[a];
                    }
                    continue;
                }
                if ( !f->w || f->w->numpoints < 3 )
                    continue;
                float centre[3] = { 0.0f, 0.0f, 0.0f };
                for ( int p = 0; p < f->w->numpoints; ++p )
                    for ( int a = 0; a < 3; ++a )
                        centre[a] += f->w->p[p][a] / (float)f->w->numpoints;
                for ( int s = 0; s < srcDef->faceCount; ++s )
                {
                    const face_t *sf = &srcDef->faces[s];
                    if ( s == faceIndex || Dot3( sf->plane.normal, f->plane.normal ) < 0.999999f
                      || fabsf( Dot3( sf->plane.normal, centre ) - sf->plane.dist ) > 0.01f )
                        continue;
                    for ( int k = 0; k < 3; ++k )   // a side in a source face's plane
                        Copy3( sf->planepts[k], f->planepts[k] );
                    break;
                }
            }
            KiwiValid_Rebuild( def );
            const char *why = 0;
            if ( !KiwiValid_CheckBrush( def, &why ) )
            {
                for ( int c = 0; c < def->faceCount; ++c )   // keep the as-built planes
                    memcpy( def->faces[c].planepts, &saved[(size_t)c * 9], sizeof( float ) * 9 );
                KiwiValid_Rebuild( def );
            }
        }

        // KIWI (2026-09-28): diagnostics for a face whose prism will not build - the source
        // brush (full-precision planepts, faces, windings), the profile, and the rebuilt
        // prism's planes / windings, appended to %TEMP%\kiwi_extrude_fail.txt (outside the
        // repo) so the float32 replica can rebuild the exact case.
        static void DumpGrowFailure( const selbrush_t *node, int face, const kconPlane_t &plane,
                                     const std::vector<float> &loop, float dist, const char *why )
        {
            const char *tmp = getenv( "TEMP" );
            char path[512];
            _snprintf( path, sizeof( path ), "%s\\kiwi_extrude_fail.txt", tmp ? tmp : "." );
            path[sizeof( path ) - 1] = '\0';
            FILE *f = fopen( path, "a" );
            if ( !f )
                return;
            fprintf( f, "=== extrude failure: %s  face %i  dist %.9g\n", why ? why : "?", face, dist );
            if ( node && node->def && node->def->faces )
            {
                const brush_t *d = node->def;
                fprintf( f, "source brush %i faces\n", d->faceCount );
                for ( int i = 0; i < d->faceCount; ++i )
                {
                    const face_t *fc = &d->faces[i];
                    fprintf( f, " ( %.9g %.9g %.9g ) ( %.9g %.9g %.9g ) ( %.9g %.9g %.9g )  n %.9g %.9g %.9g d %.9g  w %i%s\n",
                             fc->planepts[0][0], fc->planepts[0][1], fc->planepts[0][2],
                             fc->planepts[1][0], fc->planepts[1][1], fc->planepts[1][2],
                             fc->planepts[2][0], fc->planepts[2][1], fc->planepts[2][2],
                             fc->plane.normal[0], fc->plane.normal[1], fc->plane.normal[2],
                             (double)fc->plane.dist, fc->w ? fc->w->numpoints : 0,
                             i == face ? "  <== SOURCE" : "" );
                    if ( i == face && fc->w )
                        for ( int p = 0; p < fc->w->numpoints; ++p )
                            fprintf( f, "     w[%i] %.9g %.9g %.9g\n", p,
                                     fc->w->p[p][0], fc->w->p[p][1], fc->w->p[p][2] );
                }
            }
            fprintf( f, "plane origin %.9g %.9g %.9g  n %.9g %.9g %.9g  u %.9g %.9g %.9g  v %.9g %.9g %.9g\n",
                     plane.origin[0], plane.origin[1], plane.origin[2],
                     plane.normal[0], plane.normal[1], plane.normal[2],
                     plane.u[0], plane.u[1], plane.u[2], plane.v[0], plane.v[1], plane.v[2] );
            for ( size_t i = 0; i + 1 < loop.size(); i += 2 )
                fprintf( f, "  loop %.9g %.9g\n", loop[i], loop[i + 1] );
            if ( loop.size() >= 6 )
            {
                const char *w2 = 0;
                brush_t *def = KiwiExtrude_BuildPrismDef( plane, &loop[0], (int)( loop.size() / 2 ),
                                                          0.0f, dist, &w2 );
                if ( def )
                {
                    KiwiValid_Rebuild( def );
                    fprintf( f, "prism %i faces  bounds %.9g %.9g %.9g .. %.9g %.9g %.9g\n", def->faceCount,
                             def->mins[0], def->mins[1], def->mins[2],
                             def->maxs[0], def->maxs[1], def->maxs[2] );
                    for ( int i = 0; i < def->faceCount; ++i )
                    {
                        const face_t *fc = &def->faces[i];
                        fprintf( f, " ( %.9g %.9g %.9g ) ( %.9g %.9g %.9g ) ( %.9g %.9g %.9g )  n %.9g %.9g %.9g d %.9g  w %i\n",
                                 fc->planepts[0][0], fc->planepts[0][1], fc->planepts[0][2],
                                 fc->planepts[1][0], fc->planepts[1][1], fc->planepts[1][2],
                                 fc->planepts[2][0], fc->planepts[2][1], fc->planepts[2][2],
                                 fc->plane.normal[0], fc->plane.normal[1], fc->plane.normal[2],
                                 (double)fc->plane.dist, fc->w ? fc->w->numpoints : 0 );
                    }
                    Brush_Free_R( def );         // never linked
                }
                else
                    fprintf( f, "prism build refused: %s\n", w2 ? w2 : "?" );
            }
            fclose( f );
            Sys_Printf( "Extrude Face: failure details appended to %s\n", path );
        }

        // Build and validate all unlinked defs before landing them with one undo bracket.
        void Grow()
        {
            // Every selected face grows by one depth along its own normal.
            // Build and gate every prism before landing any; failures are per face.
            const char *why = "unknown";
            std::vector<brush_t *> defs;
            int failed = 0;
            const int n = (int)( m_loop.size() / 2 );
            {
                const char *w = "unknown";
                if ( brush_t *def = BuildGrowPrism( m_plane, m_loop, m_dist, &w ) )
                {
                    SeatCapsOnSource( def, m_node, m_faceIndex, m_dist );
                    defs.push_back( def );
                }
                else
                {
                    ++failed;
                    why = w;
                    Sys_Printf( "Extrude Face: the active face (%i-gon) was skipped — %s.\n",
                                n, w ? w : "invalid geometry" );
                    DumpGrowFailure( m_node, m_faceIndex, m_plane, m_loop, m_dist, w );
                }
            }
            for ( size_t i = 0; i < m_extraFaces.size(); ++i )
            {
                const faceSrc_t &fs = m_extraFaces[i];
                if ( fs.loop.size() < 6 )
                    continue;
                const char *w = "unknown";
                if ( brush_t *def = BuildGrowPrism( fs.plane, fs.loop, m_dist, &w ) )
                {
                    SeatCapsOnSource( def, fs.node, fs.face, m_dist );
                    defs.push_back( def );
                }
                else
                {
                    ++failed;
                    why = w;
                    Sys_Printf( "Extrude Face: selected face %i (%i-gon) was skipped — %s.\n",
                                (int)i + 2, (int)( fs.loop.size() / 2 ), w ? w : "invalid geometry" );
                    DumpGrowFailure( fs.node, fs.face, fs.plane, fs.loop, m_dist, w );
                }
            }
            if ( defs.empty() )
            {
                Sys_Printf( "Extrude Face: rejected — %s.\n",
                            why ? why : "invalid geometry" );
                return;
            }

            Select_Deselect( 1 );
            KiwiCmd_UndoBegin( "extrude face" );
            for ( size_t i = 0; i < defs.size(); ++i )
                LandDef( defs[i] );            // lands SELECTED

            // Source brushes remain untouched; rebuild typed selection from newly landed brushes.
            Sel_Clear( KiwiSel() );
            Sel_RebuildFromLegacy();

            Sys_Printf( "Extrude Face: %i new brush(es) from %i face(s) (%i-gon drive)%s.\n",
                        (int)defs.size(), (int)m_extraFaces.size() + 1, n,
                        failed ? ", some faces skipped (see above)" : "" );
            g_nUpdateBits = -1;

            // Match paste/clone handoff: start a paused Move for the selected new bodies.
            KiwiCmd_StartDeferred( KIWI_CMD_MOVE, true );
        }

        // Extra source with its own plane and CCW plane-space profile.
        struct faceSrc_t
        {
            selbrush_t        *node;
            int                face;
            kconPlane_t        plane;
            std::vector<float> loop;
        };
        std::vector<faceSrc_t> m_extraFaces;

        kconPlane_t        m_plane;
        std::vector<float> m_loop;                       // CCW, plane space
        float              m_ref[3]  = { 0.0f, 0.0f, 0.0f };
        float              m_start   = 0.0f;
        bool               m_haveStart = false;
        bool               m_have    = false;
        float              m_dist    = 0.0f;
        bool               m_axisBlocked = false;   // view gate
        bool               m_hasNum  = false;
        float              m_numWorld = 0.0f;
        bool               m_invalid = true;
        // True only for the current frame when a major grid line supplied the depth.
        bool               m_gridMajor = false;
        // Ctrl edge detector and current absolute-mode state.
        bool               m_absPrev   = false;
        bool               m_absolute  = false;
        snap_result_t      m_snap;
        char               m_hud[192] = { 0 };

        // Source face and untouched inward depth used by negative extrusion.
        selbrush_t        *m_node      = 0;
        int                m_faceIndex = -1;
        float              m_depth     = 0.0f;   // thickness along the face normal
        bool               m_haveDepth = false;
        int                m_mode      = KEXTF_IDLE;
    };

    KiwiExtrudeCommand     s_extrude;
    KiwiExtrudeFaceCommand s_extrudeFace;
}

// Pick active selected face, then another selected face, then hovered face.
// The active source drives multi-face extrusion.
bool KiwiExtrudeFace_Pick( selbrush_t **outNode, int *outFace )
{
    const selection_t &sel = KiwiSel();
    const sel_item_t  &act = sel.active;

    const sel_item_t *chosen = 0;
    if ( act.kind == SEL_FACE && Sel_BrushLive( act.brush ) && !act.brush->patch )
        chosen = &act;
    for ( size_t i = 0; i < sel.items.size() && !chosen; ++i )
    {
        const sel_item_t &it = sel.items[i];
        if ( it.kind == SEL_FACE && Sel_BrushLive( it.brush ) && !it.brush->patch )
            chosen = &sel.items[i];
    }

    pick_result_t hov;
    if ( !chosen )
    {
        hov = KiwiHover_Get();
        if ( hov.valid && hov.item.kind == SEL_FACE && Sel_BrushLive( hov.item.brush )
          && !hov.item.brush->patch )
            chosen = &hov.item;
    }
    if ( !chosen )
        return false;

    const brush_t *def = chosen->brush->def;
    if ( !def || !def->faces || chosen->faceIndex < 0 || chosen->faceIndex >= def->faceCount )
        return false;
    if ( outNode ) *outNode = chosen->brush;
    if ( outFace ) *outFace = chosen->faceIndex;
    return true;
}

bool KiwiExtrudeFace_CanExecute()
{
    selbrush_t *node = 0;
    int         face = -1;
    return KiwiExtrudeFace_Pick( &node, &face );
}

// Exported prism writer; see the header contract.
brush_t *KiwiExtrude_BuildPrismDef( const kconPlane_t &plane, const float *loopUV, int n,
                                    float lo, float hi, const char **why )
{
    const char *localWhy = "unknown";
    if ( !why )
        why = &localWhy;
    if ( !loopUV || n < 3 || n > KEXT_MAX_PROFILE )
    {
        *why = "profile out of range";
        return 0;
    }

    prismBuild_t pb;
    pb.plane = plane;
    pb.lo    = lo;
    pb.hi    = hi;

    std::vector<float> uv( loopUV, loopUV + (size_t)n * 2 );
    std::vector<int>   ring;
    ring.reserve( (size_t)n );
    for ( int i = 0; i < n; ++i )
        ring.push_back( i );

    return BuildPieceDef( pb, uv, ring, why );
}

selbrush_t *KiwiExtrude_LandDef( brush_t *def )
{
    return LandDef( def );
}

bool KiwiExtrude_CanExecute()
{
    return !KiwiRegion_All().empty();
}

void KiwiExtrude_RegisterCommands()
{
    Radiant_RegisterCommand( "KiwiExtrudeRegion", 0, 0, KIWI_CMD_EXTRUDE_REGION );
    Radiant_RegisterCommand( "KiwiExtrudeFace",   0, 0, KIWI_CMD_EXTRUDE_FACE );
}

// E dispatches to face extrusion when a face is selected or hovered, otherwise
// to region extrusion. If neither exists, choose the face command so Begin can
// print the refusal instead of failing silently in CanExecute.
KiwiEditorCommand *KiwiExtrude_CommandForId( int commandId )
{
    if ( commandId == KIWI_CMD_EXTRUDE_REGION )
        return &s_extrude;
    if ( commandId == KIWI_CMD_EXTRUDE_FACE )
    {
        if ( KiwiExtrudeFace_CanExecute() )
            return &s_extrudeFace;
        if ( KiwiExtrude_CanExecute() )
            return &s_extrude;
        return &s_extrudeFace;
    }
    return 0;
}

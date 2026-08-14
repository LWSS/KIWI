#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_loft.cpp — ROUND AF, ITEM 6.  See kiwi_loft.h for the Plasticity findings
// (cited file:line, including the two things it does NOT have), the grammar, the
// eight-step pipeline and the honest limits.
//
// NEW code over the ported cores.  Every brush that reaches the map is built with
// the same allocator / face-array / rebuild / gate / land sequence kiwi_extrude.h
// transcribed from Ed_NewBrushDrag, and the landing tail is literally
// KiwiExtrude_LandDef so there is ONE spelling of it in the editor.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"                // camera_s (the eye-orient reference)
#include <gfx_d3d/r_gfx.h>          // GfxColor, Byte4PackPixelColor
#include <gfx_d3d/r_material.h>     // Material
#include <gfx_d3d/r_rendercmds.h>   // MaterialTechniqueType, TECHNIQUE_UNLIT

#include "kiwi_loft.h"
#include "kiwi_command.h"
#include "kiwi_extrude.h"           // KiwiExtrude_LandDef (the ported creation tail)
#include "kiwi_lines.h"
#include "kiwi_material.h"
#include "kiwi_numeric.h"
#include "kiwi_pick.h"
#include "kiwi_selection.h"
#include "kiwi_units.h"
#include "kiwi_validity.h"
#include "radiant_registry.h"       // ROUND AT, ITEM 3 — the sticky CURVE settings

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <vector>

// ── ported entry points (each verified against its DEFINITION) ──────────────
extern int         Sys_Printf( const char *fmt, ... );                        // win_qe3.cpp
extern camera_s   *Ed_Camera();                                               // camwnd.cpp:153
extern int         g_nUpdateBits;                                             // 0x25D5A74 (mainfrm.cpp)
extern brush_t    *Brush_Alloc( const void *materialDefSrc, eclass_t *ecls );  // brush.cpp 0x4751e0
extern void        Brush_Free_R( brush_t *def );                              // brush.cpp 0x475af0
extern void        Select_Deselect( int bAlsoFreeFaces );                     // select.cpp 0x48E800

// brush.cpp / xywnd.cpp // KIWI-UX forwarders — the same pair kiwi_extrude.cpp:61-63
// declares, copied verbatim (a drift here is a link error, which is the point).
extern void Ed_BrushSetFaceCount( brush_t *def, int faceCount );
extern void Ed_EnsureCurrentMaterial_Kiwi();

// ── KIWI-UX (ROUND AO, ITEM 1c) — THE PATCH MEDIUM ─────────────────────────
// The CURVE bridge lands q3 bezier patches, and it does so through exactly the
// sequence kiwi_patchfillet.cpp already uses.  Every declaration below is copied
// VERBATIM from kiwi_patchfillet.cpp:45-64, comments included, so a drift between
// the two files is a link error rather than a silent second spelling.
extern selbrush_t  *Brush_AddToList( brush_t *def, entity_s *owner );         // brush.cpp:656  0x475980
extern void         Brush_AddToList2( selbrush_t *b );                        // brush.cpp:910  0x4765A0
extern patchMesh_t *MakeNewPatch();                                           // pmesh.cpp:136  0x437AC0
extern brush_t     *AddBrushForPatch( patchMesh_t *p, entity_s *world_ent );  // pmesh.cpp:840  0x4386A0
// ROUND AJ, ITEM 2 — the creation tail with the naturalize scale taken from the
// PARENT FACE's texdef instead of the editor default (pmesh.cpp:1508).
extern void         Patch_KiwiFinishNewLike( patchMesh_t *p, const texdef_sub_t *srcTex );
extern void         Patch_KiwiCapAlign( patchMesh_t *p );                     // pmesh.cpp (ROUND AN)

// The translucent-fill pair, declared exactly as kiwi_extrude.cpp:69-76,
// kiwi_region.cpp:92-98, kiwi_split.cpp:68-72 and kiwi_boolean.cpp:56-61 declare
// it.  R_AddCmdSetMaterialColor comes from r_rendercmds.h.
extern char        Byte4PackPixelColor( float *from, GfxColor *out );          // 0x402ac0
extern void        __cdecl R_AddRenderCmdDrawTris(
                       Material *material, MaterialTechniqueType techType, short indexCount,
                       const uint16_t *indices, short vertexCount,
                       const float ( *xyzw )[4], const float ( *normal )[3], float *color,
                       const float ( *st )[2] );                               // 0x4fd1c0

namespace
{
    // ── the numeric field table (kiwi_command.h NumericFields) ──────────────
    // STATIC storage — the numeric layer copies the structs but never the labels.
    // TWO fields, so Tab cycles them (kiwi_command.cpp's rung 0 only offers Tab to
    // the command when the field count is <= 1, and this command wants the numeric
    // layer's own cycling instead).  Neither is a LENGTH: `density` is a count and
    // `tension` a bare factor, so both have to undo Units_FromDisplay the way
    // kiwi_primitive.cpp:232-252 does for its side count.
    const kiwiNumField_t KLOFT_FIELDS[2] =
    { { "density", KNUM_COUNT,  false },
      { "tension", KNUM_FACTOR, false } };

    // Preview colours.  The blue is kiwi_extrude.cpp's KEXT_COL_PREVIEW verbatim —
    // "a new body is coming" is the same statement there and here — and the red is
    // its KEXT_COL_BAD, likewise, so an invalid loft reads exactly like an invalid
    // extrude.  The yellow is the one every grabbable / clickable thing in this
    // layer uses (kiwi_gizmo.cpp KGZ_HOT, kiwi_boolean.cpp KBOOL_HOT_LINE).
    const float KLOFT_COL_PREVIEW[3] = { 0.55f, 0.85f, 1.00f };
    const float KLOFT_COL_BAD    [3] = { 1.00f, 0.30f, 0.25f };
    const float KLOFT_COL_HOT    [3] = { 1.00f, 0.90f, 0.30f };
    const float KLOFT_COL_ENDS   [3] = { 0.35f, 1.00f, 0.55f };   // the two SOURCE rings
    const float KLOFT_FILL_OK  [4] = { 0.55f, 0.85f, 1.00f, 0.22f };
    const float KLOFT_FILL_BAD [4] = { 1.00f, 0.30f, 0.25f, 0.22f };

    const float KLOFT_EPS = 1.0e-6f;

    // ROUND AO, ITEM 1(c): "is this point on that face's plane".  0.01 world units
    // — the same on-plane tolerance kiwi_boolean.cpp's KiwiBool_PointInSolid uses
    // for the same question, and the same order as KVALID_PLANE_DIST.
    const float KLOFT_ONPLANE_EPS = 0.01f;

    // ROUND AO, ITEM 1(c): a span's arc handle is refused and replaced by the
    // midpoint when the two tangent lines meet further out than this multiple of
    // the span itself.  Two nearly parallel tangents intersect arbitrarily far
    // away, and a control point out there is a patch that swings off to infinity;
    // 2.0 admits every sane bend (a 90-degree quarter arc's handle sits at about
    // 0.71 of the chord from either end) and refuses the pathological ones.
    const float KLOFT_HANDLE_MAX = 2.0f;

    // ── vector helpers, spelled as every other file in this layer spells them ──
    inline float Dot3( const float *a, const float *b )
    { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
    inline void Copy3( const float *a, float *o ) { o[0]=a[0]; o[1]=a[1]; o[2]=a[2]; }
    inline void Sub3( const float *a, const float *b, float *o )
    { o[0]=a[0]-b[0]; o[1]=a[1]-b[1]; o[2]=a[2]-b[2]; }
    inline void Mad3( const float *a, const float *d, float s, float *o )
    { o[0]=a[0]+d[0]*s; o[1]=a[1]+d[1]*s; o[2]=a[2]+d[2]*s; }
    inline void Cross3( const float *a, const float *b, float *o )
    { o[0]=a[1]*b[2]-a[2]*b[1]; o[1]=a[2]*b[0]-a[0]*b[2]; o[2]=a[0]*b[1]-a[1]*b[0]; }
    inline float Len3( const float *a ) { return sqrtf( Dot3( a, a ) ); }
    inline bool Norm3( float *v )
    {
        const float l = Len3( v );
        if ( l < KLOFT_EPS ) return false;
        v[0] /= l; v[1] /= l; v[2] /= l;
        return true;
    }

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

    // Newell's method — the area-weighted normal of a (possibly non-planar) ring.
    // It is the right tool twice over here: it is stable for a nearly-flat ring
    // where three-point cross products are not, and its magnitude is twice the
    // projected area, so a degenerate ring reports itself by collapsing.  Returns
    // false when there is no direction to be had.
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

    // The ring's best-fit plane (Newell normal through the centroid) — and, in the
    // same call, the PROJECTION of every point onto it.  Step 6 of the pipeline; see
    // kiwi_loft.h for why the projection is what makes the segments mate.
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

    // Step 3 — COUNTS.  Split the ring's LONGEST edge, repeatedly, until it has
    // `target` points.  Splitting the longest edge (rather than resampling the whole
    // ring at uniform arc length) is what keeps every original corner of the source
    // face: the points this adds are strictly collinear with an existing edge, so the
    // profile's SHAPE is bit-for-bit unchanged and only its parameterisation grows.
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

    // Step 4 — CORRESPONDENCE.  The cyclic offset k that minimises the total squared
    // distance between the two rings once both are CENTRED and the loft-axis
    // component is projected out.  Centring removes the translation the loft is
    // supposed to have; projecting out the axis removes the length of the bridge,
    // which is not information about which corner meets which.  What is left is
    // exactly "which rotation of B lines its corners up with A's", i.e. no twist.
    //
    // O(n²) over n <= KLOFT_MAX_RING (64) = 4096 distance terms, once per
    // re-tessellation.  A closed-form angular match would be O(n log n) and would be
    // wrong for a ring whose vertices are not angularly uniform — which, after
    // SubdivideTo has split the long edges of a rectangle, is every ring here.
    // ── KIWI-UX (ROUND AO, ITEM 1): REPLACED BY BestOffset2D ────────────────
    // This function projected both rings along the STRAIGHT AXIS and compared what
    // was left.  kiwi_loft.h carries the full account of why that is the wrong
    // plane for two faces that are not parallel — it squashes each ring along a
    // different in-plane direction, inverts their aspect ratios at 90 degrees, and
    // picks a cyclic offset that is routinely one vertex out, i.e. a twist that no
    // density or mode can then rescue.  The replacement compares the rings as 2D
    // coordinates in a pair of frames the transport has already aligned, and it
    // reduces to this function's answer exactly when the two faces are parallel.
    // The body is gone rather than kept dead: a second correspondence rule in the
    // file is the duplicate-function drift this codebase keeps paying for.

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

    // ═════════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND AO, ITEM 1) — THE ROTATION-MINIMISING FRAME
    // ═════════════════════════════════════════════════════════════════════════
    // kiwi_loft.h carries the whole derivation and the failure it fixes.  These
    // are the three primitives it names.
    //
    // A DETERMINISTIC TANGENT for a plane normal.  The same "cross with whichever
    // world axis n is least aligned with" rule PlanePts below already uses, so the
    // two agree and a frame is reproducible across rebuilds.
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

    // RODRIGUES, spelled out: rotating `in` about the UNIT axis `w` by an angle
    // whose cosine and sine are given.
    //     R x = x cos + (w x x) sin + w (w.x)(1 - cos)
    void Rodrigues( const float w[3], float cosT, float sinT,
                    const float in[3], float out[3] )
    {
        float cr[3];
        Cross3( w, in, cr );
        const float d = Dot3( w, in ) * ( 1.0f - cosT );
        for ( int k = 0; k < 3; ++k )
            out[k] = in[k] * cosT + cr[k] * sinT + w[k] * d;
    }

    // The MINIMAL rotation taking `from` to `to`, evaluated at fraction `t`.
    // Both inputs must be unit.  `axisOut` is the rotation axis (needed by the
    // caller to detect the antipodal case), `cosT`/`sinT` the FULL angle's pair.
    //
    // A minimal rotation has no component about `from` itself, which is what makes
    // the transported frame twist-free — the rotation-minimising frame's defining
    // property, in closed form because this path has exactly two ends rather than a
    // sampled spine.
    //
    // Returns false when the two normals are parallel or antipodal: the caller
    // treats parallel as the IDENTITY (which is the case every loft that already
    // worked lands in) and antipodal as a refusal, because "which way round" is
    // genuinely undefined there.
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

    // A ring's coordinates in a frame, i.e. steps 4/5's working representation.
    // `out2` is 2 floats per vertex.
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

    // Step 4, RE-STATED IN THE FRAME.  Identical in shape to the old BestOffset —
    // same O(n²) sweep, same early-out — but the two rings are compared as 2D
    // coordinates in their OWN frames, which the transport has already aligned.
    // For parallel faces the two frames are the same and this is the old answer.
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

    // ── THE PLANEPTS WRITER ─────────────────────────────────────────────────
    // face_t has no independent plane field the editor trusts: Face_MakePlane
    // recomputes `plane` from planepts on every rebuild and the .map serialiser
    // writes the planepts (kiwi_extrude.h states the rule).  So every plane this
    // file decides has to be turned back into three well-spread points.
    //
    // Face_MakePlane's normal is cross( p0 - p1, p2 - p1 ).  Feeding
    //   p1 = c,  p0 = c + t*S,  p2 = c + u*S
    // makes that cross( t*S, u*S ) = S² * (t × u), so choosing an orthonormal pair
    // with t × u == n gives exactly +n.  S is a SPREAD radius rather than 1: three
    // points a unit apart on a plane 4000 units from the origin is where a float
    // cross product loses its nerve, and the .map round-trip has to survive it too.
    void PlanePts( const float n[3], float dist, const float anchor[3], float spread,
                   float out[3][3] )
    {
        // A tangent that cannot be parallel to n: cross with whichever world axis n
        // is least aligned with.
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

    // ── ONE SEGMENT ─────────────────────────────────────────────────────────
    // Pipeline step 7.  `lo` / `hi` are two corresponded, planarised, equal-count
    // rings; `loN/loD` and `hiN/hiD` their own fit planes.  The def comes back
    // UNLINKED and UNBUILT — the caller rebuilds, gates and then lands or frees, so
    // a rejection costs nothing (kiwi_extrude.h "REJECTION IS FREE").
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

        // The solid's own centre, used to orient every plane outward, and a spread
        // radius that scales with the thing being built.
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

        // face 0 — the LO cap.  Its plane is ring `lo`'s own fit plane; the outward
        // sense is "away from the other ring", which for a segment is unambiguous.
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
        // face 1 — the HI cap, the same rule against the other ring.
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

        // faces 2..n+1 — one side per corresponding edge PAIR.  This is the step
        // kiwi_loft.h calls the honest approximation: the quad
        //     lo[i], lo[i+1], hi[i+1], hi[i]
        // is not planar in general, so its Newell normal is taken as the plane's
        // direction and the plane is then PUSHED OUT to the furthest of the four
        // points.  Every ring vertex therefore lies inside every half-space, the
        // brush contains the hull of all 2n of them, and nothing the user drew is
        // ever clipped away.  The inflation is the quad's non-planarity, which is a
        // function of how much the profile twists across ONE segment — i.e. it is
        // what the density field buys down.
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
                // A collapsed quad — the two rings met at this vertex.  Fall back to
                // the edge x axis cross, which is the plane a zero-height quad would
                // have had; if that collapses too the segment is degenerate and §19
                // will say so in a sentence the user can act on.
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

        // ── MATERIALS (kiwi_material.h) ─────────────────────────────────────
        // The chosen SOURCE FACE first — that is the surface the user pointed at and
        // the one the bridge is meant to continue.  A tool material on it (the mapper
        // lofted between two caulk faces) falls through to R2/R3 over the source
        // BRUSH, which is the same ladder every other new surface in this layer
        // climbs, and ends at the classic caulk synthesis rather than inventing a
        // visible skin the mapper did not ask for.
        if ( mtlDef && mtlDef->faces && mtlFace >= 0 && mtlFace < mtlDef->faceCount )
        {
            const bool direct = KiwiMtl_FaceIsInheritable( &mtlDef->faces[mtlFace] );
            for ( int f = 0; f < def->faceCount; ++f )
            {
                if ( direct )
                    KiwiMtl_SeedFaceFrom( &def->faces[f], mtlDef, mtlFace );
                else
                {
                    // R2 wants the NEW surface's normal to score against.  It has not
                    // been computed yet (Face_MakePlane runs at rebuild), so score
                    // against the source face's own normal, which is the only stable
                    // thing on hand and is what "continue this surface" means anyway.
                    KiwiMtl_SeedClipFace( &def->faces[f], mtlDef,
                                          mtlDef->faces[mtlFace].plane.normal );
                }
            }
        }
        return def;
    }

    // ── the translucent preview fill ────────────────────────────────────────
    // Same machinery, same reasons, as kiwi_extrude.cpp EmitFilledTris and
    // kiwi_boolean.cpp FillBrush: kiwi_lines pins alpha to 1 (kiwi_lines.h TRAP 2)
    // and cannot express a translucent surface at all, so this goes through
    // R_AddRenderCmdDrawTris on g_qeglobals.d_white inside a neutral MATERIAL_COLOR
    // bracket, with the round-AA eye-orient over the whole index buffer
    // (kiwi_lines.h TRAP 3).
    //
    // ONE DRAW PER SEGMENT, not per quad: 2n verts and 2n triangles for the whole
    // sleeve.  Per quad would be up to KLOFT_MAX_SEGS * KLOFT_MAX_RING = 2048 draw
    // commands in a frame, which is the render-command pressure kiwi_lines.h TRAP 1
    // exists to keep out of this layer.
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
                // KIWI-UX (ROUND AL, ITEM 1): a CONSTANT world normal, not the
                // view's — kiwi_lines.h TRAP 4.
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

    // A brush face this verb may legally loft from.  Patches have no brush face at
    // all (kiwi_pick.cpp:584-599 refuses to resolve one) and a fixed-size entity's
    // bbox is not model geometry — the same two tests every CSG-ish path in this
    // layer runs (kiwi_boolean.cpp Usable, kiwi_split.cpp Splittable).
    bool UsableFace( const selbrush_t *b, int face )
    {
        if ( !b || !b->def || b->patch )
            return false;
        const brush_t *def = b->def;
        if ( !def->faces || face < 0 || face >= def->faceCount )
            return false;
        const winding_t *w = def->faces[face].w;
        if ( !w || w->numpoints < 3 || w->numpoints > KLOFT_MAX_RING )
            return false;
        const entity_s *owner = b->owner;
        if ( !owner || !owner->def )
            return false;
        const entity_s *ownerDef = owner->def;
        if ( !ownerDef->eclass || ownerDef->eclass->fixedsize )
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

    // ═════════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND AT, ITEM 3) — THE THREE STICKY CURVE SETTINGS
    // ═════════════════════════════════════════════════════════════════════════
    // `Reset()` runs on every `Begin()` and re-seeds every field from a constant,
    // which is right for the per-gesture ones (the two faces, the twist) and wrong
    // for a preference: a mapper building a curved wall wants two-sided and their
    // continuity choice for the whole session, not for one loft.  Stored in the
    // profile ini exactly as kiwi_camera.cpp:606 stores the ortho toggle — read
    // once, lazily, and written at the moment of the change so a crash cannot lose
    // it.  The read validates, because an ini is a text file a user can edit.
    int  s_contStart = KLOFT_CONT_G1;
    int  s_contEnd   = KLOFT_CONT_G1;
    bool s_twoSided  = false;
    bool s_prefsRead = false;

    void LoftPrefs_Read()
    {
        if ( s_prefsRead )
            return;
        s_prefsRead = true;
        s_contStart = Radiant_ProfileGetInt( KLOFT_SECTION, "ContStart", KLOFT_CONT_G1 );
        s_contEnd   = Radiant_ProfileGetInt( KLOFT_SECTION, "ContEnd",   KLOFT_CONT_G1 );
        s_twoSided  = ( Radiant_ProfileGetInt( KLOFT_SECTION, "TwoSided", 0 ) != 0 );
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
        Radiant_ProfileSetInt( KLOFT_SECTION, "TwoSided",  twoSided ? 1 : 0 );
    }

    const char *ContName( int c )
    {
        return ( c == KLOFT_CONT_G0 ) ? "G0"
             : ( c == KLOFT_CONT_G2 ) ? "G2" : "G1";
    }

    // ═════════════════════════════════════════════════════════════════════════
    //  L — LOFT.
    // ═════════════════════════════════════════════════════════════════════════
    class KiwiLoftCommand : public KiwiEditorCommand
    {
    public:
        const char *Name() const override { return "Loft"; }
        bool CanExecute() override { return KiwiLoft_CanExecute(); }

        // Stage 1 is a face PICKER, so a click must place a choice rather than
        // commit the gesture (kiwi_command.h WantsClicks).  Stage 2 keeps clicking
        // live too, so a mis-aimed second face can simply be re-clicked without
        // walking the stages back.
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

        // Neither field is a LENGTH, so both undo the numeric layer's
        // inches→world conversion — kiwi_primitive.cpp:232-252's rule, verbatim.
        void NumericFieldChanged( int field, bool has, float world ) override
        {
            if ( !has )
                return;                        // cleared: keep what the tool has
            if ( field == 0 )
            {
                // ROUND AO, ITEM 1(c): the ceiling is MODE-DEPENDENT now — CURVE is
                // bounded by the patch control grid, not by taste — so the clamp
                // goes through ClampSegs, which is the one place that knows.
                m_segs      = ClampSegs( (int)floorf( Units_ToDisplay( world ) + 0.5f ) );
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

        // ═══════════════════════════════════════════════════════════════════
        //  KIWI-UX (ROUND AI, ITEM 3) — THE OPTIONS PANEL
        // ═══════════════════════════════════════════════════════════════════
        // USER DIRECTIVE, verbatim: "The lofting is pretty cool, but make the
        // options clickable buttons like in plasticity.  Open a temp lofting panel
        // (still allows enter/rightclick completion)."
        //
        // Three rows, and every one of them is a VIEW onto a handler that already
        // existed — the panel calls what the keyboard calls and stores nothing:
        //
        //   mode     -> KeyDown( 'D' ), literally the same call the key makes, so
        //               the "a mode flip re-seeds the density unless it was typed"
        //               rule (m_segsTyped) cannot be forgotten by one path.
        //   density  -> NumericFieldChanged( 0 ), which is where the clamp AND the
        //               m_segsTyped latch live.
        //   tension  -> the panel's KOPT_NUMFIELD kind reads/writes field 1 itself.
        //
        // The panel is only offered at stage KLOFT_LIVE.  At KLOFT_PICK_B there is
        // nothing to tune yet — no second face, no stations, and Rebuild has never
        // run — so a panel there would be three dead controls plus a Confirm button
        // the command already refuses in words ("click the second face first").
        //
        // TENSION IS GATED ON THE MODE (`enabledBy = 0`) because it is meaningless
        // in RULED: Rebuild only reads m_tension on the tangent path.  Plasticity
        // greys rather than hides for the same reason
        // (plasticity/src/commands/fillet/FilletDialog.tsx:73 `class="disabled"`).
        int CommandOptions( const kiwiOption_t **out ) const override
        {
            if ( m_stage != KLOFT_LIVE )
                return 0;
            static const char *const s_modes[KLOFT_BRIDGE_COUNT] =
                { "Ruled", "Tangent", "Curve" };
            // ── KIWI-UX (ROUND AO, ITEM 1): THREE MODES AND A TWIST STEPPER ──
            // The Bridge row gains CURVE (the patch medium, kiwi_loft.h) and the
            // panel gains one row: Twist, whole-vertex steps of the ring
            // correspondence.  The density row's CEILING is now mode-dependent —
            // CURVE is bounded by the patch control grid — and the table is rebuilt
            // per call for exactly that reason (it is a `static` per invocation only
            // to satisfy the panel's "the labels outlive the call" contract, so the
            // range is patched into it before it is handed over).
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
            // ── KIWI-UX (ROUND AT, ITEM 3b/3c): THREE MORE ROWS, CURVE ONLY ──
            // Continuity per END and the two-sided emit are properties of the
            // PATCH medium and mean nothing to a brush bridge, so they are offered
            // only in CURVE mode.
            //
            // WHY THEY ARE ADDED RATHER THAN GREYED, against this panel's own
            // stated preference for a disabled row over a hidden one: `enabledBy`
            // can express exactly one predicate — "that option's value is
            // non-zero" (kiwi_cmdoptions.cpp:176-179) — and "the Bridge row is
            // CURVE" is not that (TANGENT is non-zero too).  The choice was
            // therefore between rows that are ENABLED in TANGENT where they do
            // nothing, and rows that arrive with the medium.  A live control that
            // does nothing is the worse lie, and the Bridge row is directly above
            // them saying which medium you are in.  Widening the framework's gate
            // would touch every command's option table for one row.
            static const char *const s_cont[KLOFT_CONT_COUNT] = { "G0", "G1", "G2" };
            static const kiwiOption_t s_curveTemplate[3] = {
                { "Start continuity", KOPT_ENUM,   s_cont, KLOFT_CONT_COUNT, 0,
                  0.0f, 0.0f, -1 },
                { "End continuity",   KOPT_ENUM,   s_cont, KLOFT_CONT_COUNT, 0,
                  0.0f, 0.0f, -1 },
                { "Two-sided",        KOPT_TOGGLE, 0, 0, 0,
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
            if ( opt == 4 ) return m_cont[0];      // ROUND AT, ITEM 3b
            if ( opt == 5 ) return m_cont[1];
            if ( opt == 6 ) return m_twoSided ? 1 : 0;
            return 0;
        }

        void OptionChanged( int opt, int value ) override
        {
            if ( opt == 0 )
            {
                // ROUND AO: 'D' CYCLES three modes, so the panel can no longer be a
                // "fire the key if the state differs" shim — it would need up to two
                // presses and would pick the direction for the user.  The mode is set
                // directly and everything the key does AFTER the flip is factored
                // into SetBridge, which the key now calls too, so there is still
                // exactly one place the re-seed rule lives.
                if ( value >= 0 && value < KLOFT_BRIDGE_COUNT && value != m_bridge )
                    SetBridge( value );
                return;
            }
            if ( opt == 1 )
            {
                // Through the numeric handler so the clamp and the m_segsTyped
                // latch are applied in one place.  Units_FromDisplay because that
                // handler's contract is RAW WORLD UNITS and it undoes the
                // conversion itself (see NumericFieldChanged above).
                NumericFieldChanged( 0, true, Units_FromDisplay( (float)value ) );
                return;
            }
            if ( opt == 3 )
            {
                // ROUND AO, ITEM 1(d).  Stored raw and reduced modulo the ring count
                // inside BuildStations, because the ring count is not known here (it
                // depends on both profiles and on SubdivideTo).
                m_twist = value;
                Rebuild();
                UpdateHud();
                g_nUpdateBits = -1;
                return;
            }
            // ── ROUND AT, ITEM 3b/3c ────────────────────────────────────────
            // All three write through to the profile at the moment of the change
            // (LoftPrefs_Write), so the setting survives this gesture, the next
            // command and the session.  Rebuild() re-runs because the CURVE
            // preview now shows the actual bezier (DrawWorld), so a continuity
            // change is visible before the commit.
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
                UpdateHud();
                Sys_Printf( "Loft: two-sided %s — each strip is emitted %s.%s\n",
                            m_twoSided ? "ON" : "off",
                            m_twoSided ? "TWICE, the second copy flipped"
                                       : "once, visible from one side",
                            m_twoSided ? "  (Patches carry no collision either way: "
                                         "put caulk inside the wall if the player has "
                                         "to hit it.)" : "" );
                g_nUpdateBits = -1;
            }
        }

        // ── ROUND AO, ITEM 1: the ONE place a mode change is applied ────────
        void SetBridge( int mode )
        {
            m_bridge = mode;
            // The three modes want genuinely different densities — a straight bridge
            // is ONE solid, a curved brush bridge is not readable below about eight,
            // and a patch span is already curved so two is plenty — so a mode change
            // re-seeds the count UNLESS the user has typed one.  Typing outranks the
            // tool, always (kiwi_numeric.h).  A TYPED count is still clamped, because
            // CURVE's ceiling is a format bound and not a preference.
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

        // The second face may be ANY brush face, including another face of the same
        // solid, so nothing is excluded — the only refusal is "that is face A again",
        // which Click() states in words.
        unsigned PickFlags() const override { return PICKF_NONE; }

        int HudPrompts( const kiwiPrompt_t **out ) const override
        {
            static const kiwiPrompt_t s_pick[] = {
                { "LMB", "Pick the second face" },
                { "Esc", "Cancel" },
            };
            static const kiwiPrompt_t s_live[] = {
                { "D",    "Ruled / Tangent / Curve" },   // ROUND AO — three media
                { "[ ]",  "Twist the correspondence" },  // ROUND AO, ITEM 1(d)
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

            // ── ENTRY (kiwi_loft.h THE GRAMMAR) ─────────────────────────────
            // The ACTIVE face first, then the rest of the selection in order — the
            // same "active drives, the rest follow" rule §20's push/pull uses and
            // KiwiExtrudeFace_Pick already spells (kiwi_extrude.cpp:1608-1640).
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
                Sys_Printf( "Loft: bridging the two selected faces — D switches "
                            "RULED / TANGENT, Tab picks density or tension, "
                            "RMB / Enter applies.\n" );
                return true;
            }

            m_stage = KLOFT_PICK_B;
            UpdateHud();
            Sys_Printf( "Loft: click the OTHER face — any face, on any solid "
                        "(Esc cancels).\n" );
            return true;
        }

        // THE FRAMEWORK'S PICK IS NOT USABLE HERE, for the reason kiwi_matchface.cpp
        // and kiwi_boolean.cpp both state: KiwiCmd_MouseMove builds it with the
        // user's CURRENT SELECTION MODE, and in Point / Edge / Object mode that
        // resolves to a vertex, an edge or a whole solid — never the FACE this verb
        // needs.  So the ray is re-cast under a FACE-only mask, which is exactly
        // what makes kiwi_pick.cpp's `faceGranularity` arm fire.
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
                // ROUND AO, ITEM 1: D CYCLES rather than toggles, because there are
                // three media now.  Everything that used to follow the flip lives in
                // SetBridge, which the options panel calls too.
                SetBridge( ( m_bridge + 1 ) % KLOFT_BRIDGE_COUNT );
                return true;
            }
            // ── ROUND AO, ITEM 1(d): the TWIST stepper on the keyboard ──────
            // [ and ] are the pair this layer already uses for a whole-step count
            // (kiwi_primitive.h's side count), so a mapper who knows one knows this.
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
                // Walk back to the picker rather than cancelling outright — the same
                // ladder the cut tool and the boolean give Esc, and for the same
                // reason (the expensive part of the gesture was choosing face A).
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

        // ROUND AF, ITEM 3's hook.  A 32-segment loft over a 64-gon is 32*65*2
        // segments of outline, which is four times the framework's default batch —
        // the same class of "the cost is a function of a field the user is turning"
        // the boolean's tool set is, and answered the same way.
        int LineBudget() const override
        {
            int n = 96;                        // the snap marker's headroom
            if ( !m_stations.empty() )
            {
                const int ring = RingN( m_stations[0] );
                n += (int)m_stations.size() * ring;        // every station ring
                // ROUND AT, ITEM 3a: a CURVE rail is drawn as the quadratic it will
                // become, so each span costs KLOFT_CURVE_PREVIEW segments instead
                // of one.  Asked of the mode rather than budgeted for the worst
                // case: a 128-span RULED bridge must not pay for a curve it never
                // draws.
                const int perSpan = UsePatches() ? KLOFT_CURVE_PREVIEW : 1;
                n += ( (int)m_stations.size() - 1 ) * ring * perSpan;
            }
            return n;
        }

        void DrawWorld() override
        {
            // The two SOURCE rings are always drawn, in both stages: in stage 1 they
            // are the only thing that says which face L latched, and in stage 2 they
            // are the ends the preview is anchored to.
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
            // The RAILS — one polyline per corresponding vertex, which is what makes
            // a twist visible before the user commits to it.
            //
            // ── KIWI-UX (ROUND AT, ITEM 3a): IN CURVE MODE THE RAIL IS THE CURVE ──
            // A straight rail between two stations is an honest picture of a BRUSH
            // bridge (that IS what gets built) and a lie about a patch one: the
            // committed strip is a quadratic through the span handle.  Drawing the
            // straight chord is also precisely what hid this round's bug — the
            // preview looked identical whether the handle curved or collapsed onto
            // the chord.  In CURVE mode each span is therefore evaluated as the
            // quadratic Bezier it will become, with the SAME SpanTangent/ArcHandle
            // the commit uses, so a continuity change is visible before Enter.
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
            // Nothing to restore: the preview is a DRAW, every segment is built out
            // of unlanded defs inside Apply, and no bracket exists until then.
            Reset();
            g_nUpdateBits |= 1;
        }

    private:
        enum stage_t { KLOFT_PICK_B = 0, KLOFT_LIVE };

        // ── ROUND AO, ITEM 1: the two questions the modes answer ────────────
        // "Is the PATH curved" — TANGENT and CURVE both are; RULED is not.
        bool UseTangent() const { return m_bridge != KLOFT_BRIDGE_RULED; }
        // "Is the MEDIUM patches" — only CURVE is.
        bool UsePatches() const { return m_bridge == KLOFT_BRIDGE_CURVE; }

        int DefaultSegs() const
        {
            if ( m_bridge == KLOFT_BRIDGE_CURVE )   return KLOFT_DEF_SEGS_CURVE;
            if ( m_bridge == KLOFT_BRIDGE_TANGENT ) return KLOFT_DEF_SEGS_TANGENT;
            return KLOFT_DEF_SEGS_RULED;
        }

        // The density ceiling is MODE-DEPENDENT: brush modes take round AN's 128,
        // CURVE takes the patch format's control-grid bound (kiwi_loft.h).
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

        // CURVE mode's whole gate.  A patch has no §19 to fail, so the only things
        // that can refuse it are structural.
        bool CurveRefusal( const char **why ) const
        {
            if ( m_stations.size() < 2 )
            { *why = "no stations"; return false; }
            const int n = RingN( m_stations[0] );
            if ( n < 3 )
            { *why = "the profile has fewer than three corners"; return false; }
            const int segs = (int)m_stations.size() - 1;
            if ( segs * 2 + 1 > 15 )
            { *why = "too many spans for a patch control grid — lower the density"; return false; }
            if ( n > KLOFT_MAX_RING )
            { *why = "the profiles have more corners than the ring cap"; return false; }
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
            m_twist     = 0;                  // ROUND AO, ITEM 1(d)
            // ROUND AT, ITEM 3: the CURVE settings are PREFERENCES and re-seed
            // from the store, not from a constant — see LoftPrefs_Read.
            LoftPrefs_Read();
            m_cont[0]    = s_contStart;
            m_cont[1]    = s_contEnd;
            m_twoSided   = s_twoSided;
            m_haveEndTan = false;
            m_ok        = false;
            m_fellBack  = false;              // ROUND AO, ITEM 1(a)
            m_why       = "no loft yet";
            m_stations.clear();
            m_edgeFace.clear();
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

        // ── STEPS 1..6, i.e. everything up to the brushes ───────────────────
        // Run on every state change (mode, density, tension, a re-picked face) so
        // the preview IS the result — Plasticity's own live-update shape
        // (LoftCommand.ts:19-21), with the factory replaced by this.
        //
        // ── KIWI-UX (ROUND AO, ITEM 1): SPLIT OUT OF Rebuild ────────────────
        // Rebuild is now steps 1..8: this function, and then the DRY BUILD that
        // makes the preview honest.  Splitting them is what lets the dry build's
        // TANGENT->RULED retry re-run the stations without recursing.
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

            // The two path tangent DIRECTIONS: out of A toward B, and out of B toward
            // A.  Taken from the source faces' own plane normals rather than from the
            // rewound Newell normals, because THAT is what "leaves along the face's
            // normal" means and it is the one thing G1 is a statement about.
            float outA[3], outB[3];
            Copy3( m_a->def->faces[m_aFace].plane.normal, outA );
            Copy3( m_b->def->faces[m_bFace].plane.normal, outB );
            if ( Dot3( outA, axis ) < 0.0f ) { outA[0]=-outA[0]; outA[1]=-outA[1]; outA[2]=-outA[2]; }
            if ( Dot3( outB, axis ) > 0.0f ) { outB[0]=-outB[0]; outB[1]=-outB[1]; outB[2]=-outB[2]; }

            // ── KIWI-UX (ROUND AT, ITEM 3a): KEEP THEM ──────────────────────
            // These two were locals, and throwing them away is why CURVE mode
            // came out flat: with no end tangent to hand it, `SpanTangent` fell
            // back to a one-sided difference along the span's own chord and the
            // arc handle landed ON the chord (kiwi_loft.h).  BOTH are stored
            // pointing FORWARD along the path — `outB` is negated here for the
            // same reason the Hermite negates it below.
            Copy3( outA, m_endTan[0] );
            for ( int kk = 0; kk < 3; ++kk )
                m_endTan[1][kk] = -outB[kk];
            m_haveEndTan = true;

            // 3. COUNTS.
            int n = RingN( A ) > RingN( B ) ? RingN( A ) : RingN( B );
            if ( n > KLOFT_MAX_RING )
            { m_why = "the profiles have more corners than the ring cap"; return false; }
            SubdivideTo( &A, n );
            SubdivideTo( &B, n );

            // ═══════════════════════════════════════════════════════════════
            //  KIWI-UX (ROUND AO, ITEM 1) — THE FRAMES
            // ═══════════════════════════════════════════════════════════════
            // kiwi_loft.h carries the derivation and the perpendicular-face
            // failure this replaces.  In one line: A's frame is built, and B's is
            // that frame carried across by the MINIMAL rotation nA -> nB, so the
            // two are twist-free relative to each other and the comparison in
            // step 4 is between two shapes that have not been squashed.
            float uA[3], vA[3], uB[3], vB[3];
            if ( !FrameTangent( nA, uA ) )
            { m_why = "a source winding is degenerate"; return false; }
            Cross3( nA, uA, vA );
            Norm3( vA );

            float rotAxis[3], rotCos = 1.0f, rotSin = 0.0f;
            const bool rotates = MinRotation( nA, nB, rotAxis, &rotCos, &rotSin );
            if ( !rotates && rotCos < 0.0f )
            {
                // ANTIPODAL: the two sections face exactly opposite ways after the
                // sense rewind, so "which way round" is undefined and any answer
                // would be an arbitrary twist.  Refused in words rather than
                // guessed — the user can nudge one face or use the twist stepper on
                // a shape that is not exactly back to back.
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
                Copy3( uA, uB );            // parallel: the identity, i.e. round AF
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
                // ROUND AO, ITEM 1(d): whole-vertex steps on top of the computed
                // offset.  Modulo n and normalised positive so a negative step and
                // RotateRing's forward-only walk agree.
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

                // ── THE PATH POINT, i.e. where this station's frame sits ─────
                // RULED walks the straight line between the two centroids; TANGENT
                // (and CURVE, which shares its path) walks the cubic Hermite that
                // leaves A along A's outward normal and arrives at B along B's.
                // Only the CENTROID takes the Hermite now — the section is carried
                // by the frame, which is what stops the profile pinching.
                float c[3];
                if ( !tangent )
                {
                    for ( int kk = 0; kk < 3; ++kk )
                        c[kk] = cA[kk] + ( cB[kk] - cA[kk] ) * t;
                }
                else
                {
                    // Cubic Hermite, the textbook basis:
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

                // ── THE FRAME AT t ───────────────────────────────────────────
                // The same minimal rotation, evaluated at fraction t of its angle.
                // At t == 0 it is the identity and at t == 1 it is the whole
                // rotation, so station 0 IS ring A and station `segs` IS ring B,
                // exactly — which is what keeps the loft's ends coplanar with the
                // faces it was asked to bridge.
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

                // ── THE SECTION AT t: the 2D blend, mapped through the frame ──
                // Congruent profiles therefore stay congruent at every station —
                // "the curve needs to be the same thickness" — and the ring is
                // PLANAR BY CONSTRUCTION, so step 6 below is exact.
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
                    // The ends are the SOURCE windings and are already planar; taking
                    // their own plane rather than a refit keeps the loft's end caps
                    // exactly coplanar with the faces it was asked to bridge.
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

            // ── ROUND AO, ITEM 1(c): WHICH FACE EACH PROFILE EDGE CONTINUES ──
            // Only CURVE mode reads this, but it is resolved HERE because ring A
            // is in scope here and nowhere else.  Edge i runs from A[i] to
            // A[i+1]; the face of brush A that continues it is the one (other than
            // the source face) whose plane contains BOTH endpoints.
            ResolveEdgeFaces( A );

            m_ok  = true;
            m_why = "";
            return true;
        }

        // ── ROUND AO, ITEM 1(c) ─────────────────────────────────────────────
        // For each profile edge, the index of the face of brush A the swept strip
        // grows out of, or -1.  A brush face's plane contains an edge of an
        // ADJACENT face exactly when both of that edge's endpoints satisfy the
        // plane equation, so this is two dot products per candidate — no winding
        // walk and no shared-edge bookkeeping.  The epsilon is the editor's own
        // on-plane tolerance (kiwi_boolean.cpp's KiwiBool_PointInSolid uses 0.01
        // for the same question).
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

        // ═════════════════════════════════════════════════════════════════════
        //  KIWI-UX (ROUND AO, ITEM 1a) — THE PREVIEW RUNS THE COMMIT
        // ═════════════════════════════════════════════════════════════════════
        // USER REPORT, verbatim: "dont make the preview blue until it will
        // actually work (misleading! and a time waster!)".
        //
        // It was blue whenever STATIONS existed, because steps 7 and 8 — build
        // every segment, gate it against §19 — ran only inside Apply.  They run
        // here now as a DRY RUN: every def is built, gated and FREED, which costs
        // nothing to reject (kiwi_extrude.h's "REJECTION IS FREE"; kiwi_boolean's
        // KiwiBool_WouldCarve is the same dry run one subsystem over).
        //
        // AND IT PREVIEWS WHAT WILL ACTUALLY BE BUILT.  Apply's TANGENT->RULED
        // retry is run here too, so when the retry is what will happen the stations
        // left behind are the RULED ones and the preview shows the straight bridge
        // the commit is going to make — not the curved one it is not.  `m_fellBack`
        // carries that into the HUD so the swap is announced BEFORE the click, not
        // after it.
        //
        // CURVE mode does not go through §19 at all: a patch is not a brush and
        // has no half-space validity to fail.  Its gate is the control-grid bound
        // and the station count, both checked in ClampSegs / CurveRefusal.
        void Rebuild()
        {
            m_fellBack = false;

            if ( !BuildStations() )
                return;                          // m_ok is already false, m_why set

            if ( m_bridge == KLOFT_BRIDGE_CURVE )
            {
                const char *why = 0;
                if ( !CurveRefusal( &why ) )
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
                // The retry, previewed.  m_bridge is moved rather than a flag being
                // threaded through BuildStations, because BuildStations reads the
                // mode through UseTangent() and nothing else.
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

        // Build every segment def, gate it, and report — WITHOUT landing anything.
        // Split out from Apply so the TANGENT->RULED fallback below can run the whole
        // thing twice and still land at most one set (kiwi_loft.h step 8's
        // all-or-nothing rule).
        bool BuildAll( std::vector<brush_t *> *out, const char **why )
        {
            out->clear();
            if ( m_stations.size() < 2 )
            { *why = m_why[0] ? m_why : "no stations"; return false; }

            // MATERIALS: nearest end wins per segment (kiwi_loft.h).  The midpoint
            // rounds toward face A, which is the face the user chose FIRST and the one
            // the gesture is anchored on.
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
            // ── KIWI-UX (ROUND AO, ITEM 1a): THE DECISION IS ALREADY MADE ───
            // Rebuild has run the WHOLE build as a dry run, including the
            // TANGENT->RULED retry, and has left `m_stations` holding exactly what
            // is going to be built.  So this function no longer re-derives
            // anything: it refuses when the preview said it would refuse, using the
            // same words the HUD has been showing, and otherwise builds the
            // stations that are there.
            //
            // THAT IS THE WHOLE POINT OF THE HONEST PREVIEW.  A commit that could
            // reach a verdict the preview did not show would put the two back out
            // of step, which is the bug this item is about.
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
                // Defensive only: the dry run in Rebuild built this same set from
                // these same stations one state change ago.  If it ever fires, the
                // preview and the commit HAVE diverged and saying so is worth more
                // than a silent retry.
                Sys_Printf( "Loft: refused at commit — %s.  Nothing was created.\n",
                            why ? why : "invalid geometry" );
                return;
            }

            if ( m_fellBack )
                Sys_Printf( "Loft: the TANGENT bridge was rejected, so the RULED "
                            "bridge shown in the preview was built — raise the "
                            "density and re-run if you wanted the curve.\n" );

            // ── UNDO (kiwi_extrude.h's ordering, for the same reason) ───────
            // The bracket opens over an EMPTY selection and the new brushes land
            // selected, so KiwiCmd_UndoCommit's Undo_EndBrushList stamps precisely
            // them and one Ctrl+Z removes the whole bridge.  The two SOURCE brushes
            // are never in the bracket because they are never touched.
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

        // ═════════════════════════════════════════════════════════════════════
        //  KIWI-UX (ROUND AO, ITEM 1c) — THE CURVE BRIDGE: ONE PATCH PER EDGE
        // ═════════════════════════════════════════════════════════════════════
        // kiwi_loft.h states the design (why a strip per edge, where the thickness
        // comes from, which alignment, and the control-grid bound).  The LANDING
        // SEQUENCE below is kiwi_patchfillet.cpp:1441-1559 verbatim in order —
        // MakeNewPatch -> width/height/type -> contents/flags -> ctrl -> texture /
        // lightmap -> KiwiMtl_RealizePatch -> Patch_KiwiFinishNewLike ->
        // Patch_KiwiCapAlign -> AddBrushForPatch -> Brush_AddToList ->
        // Brush_AddToList2 — because there is one right order and it is already
        // written down once.
        //
        // THE ARC HANDLE.  Column 2s sits on station s.  Column 2s+1 is the point
        // where the two stations' PATH TANGENTS meet, which is what makes the
        // quadratic span an arc rather than a chord — kiwi_patchfillet.cpp's
        // ArcPoint rule (r/cos(a/2) at a known circle) generalised to a path whose
        // curvature is not known in closed form.  The tangent at station s for
        // rail i is the central difference of the neighbouring stations' rail
        // points — and at the two END stations it is the SOURCE FACE'S NORMAL
        // (ROUND AT, ITEM 3a; the one-sided difference this used to take is the
        // span's own chord, which collapsed the quadratic to a straight line and is
        // the whole "it does not curve" report — see kiwi_loft.h).  The meeting
        // point is the closest approach of the two tangent LINES, solved as a 2x2
        // least squares:
        //
        //     minimise | (P0 + a T0) - (P1 - b T1) |²
        //       =>  [  T0.T0   T0.T1 ] [a]   [ T0.D ]        D = P1 - P0
        //           [  T0.T1   T1.T1 ] [b] = [ T1.D ]
        //
        // The handle is the midpoint of the two closest points.  Parallel tangents
        // make the determinant vanish and a negative a or b means the lines meet
        // BEHIND the span; both fall back to the chord midpoint, which is exactly
        // what a straight span wants.
        void SpanTangent( int s, int i, float out[3] ) const
        {
            const int last = (int)m_stations.size() - 1;

            // ── KIWI-UX (ROUND AT, ITEM 3a): THE END STATIONS' TANGENTS ─────
            // At s == 0 and s == last the central difference below collapses to a
            // ONE-SIDED difference, which is the span's own chord — and an arc
            // handle solved from a tangent lying on the chord IS the chord.  That
            // is the whole "it does not curve" report (kiwi_loft.h).  The correct
            // tangent at an end is the source face's own normal, which is exactly
            // what G1 means and what the Hermite path already uses; the section is
            // carried rigidly by the rotation-minimising frame, so every RAIL of
            // the ring shares that one path direction.
            //
            // G0 keeps the old one-sided behaviour ON PURPOSE — it is the setting
            // that says "meet the face at a crease", and it is exact for a
            // straight run.  G2 uses the same direction as G1 and differs only in
            // where ArcHandle puts the handle ALONG it.
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

        // ── KIWI-UX (ROUND AT, ITEM 3b): THE ARC-FIT HANDLE ─────────────────
        // The quadratic control point a CIRCLE would use for this span, solved
        // from ONE end's tangent: the tangent intersection of a circular arc from
        // P0 to P1 leaving along t0 sits at
        //     P0 + t0 * ( |chord| / ( 2 cos(angle between t0 and the chord) ) )
        // which is kiwi_patchfillet.cpp:217-234's `r / cos(alpha/2)` written for a
        // path whose radius is not known: for a quarter turn of radius r the two
        // expressions give the same point.  `from1` solves it from the FAR end
        // instead (the last span, whose G2 end is P1), by symmetry.
        // Returns false when the tangent does not lean toward the far end enough
        // for the arc to exist — the caller then takes the ordinary tangent
        // intersection, and that its own chord-midpoint fallback.
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

        // ROUND AT, ITEM 3b: `arcFrom` selects the ARC-FIT handle for a span whose
        // start (1) or end (2) is a G2 end; 0 is round AO's tangent intersection.
        // The arc fit is TRIED first and falls through to the intersection, so a
        // G2 end whose geometry cannot support an arc silently behaves as G1
        // rather than producing a shape nobody asked for.
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

        // ── ROUND AT, ITEM 3b ───────────────────────────────────────────────
        // Which end (if any) this span's handle is arc-fitted from.  Only the two
        // END spans can be, because only they touch a source face; everything in
        // between keeps round AO's tangent intersection, which is already tangent
        // continuous with its neighbours on both sides.  With ONE span and BOTH
        // ends set to G2 the span cannot be two different circles, so the START
        // wins — stated here rather than left to the reading order.
        int SpanArcFrom( int s, int segs ) const
        {
            if ( s == 0 && m_cont[0] == KLOFT_CONT_G2 )
                return 1;
            if ( s == segs - 1 && m_cont[1] == KLOFT_CONT_G2 )
                return 2;
            return 0;
        }

        // ── ROUND AT, ITEM 3 ────────────────────────────────────────────────
        // ONE strip's control grid.  Even columns sit on the stations, odd columns
        // are the span handle (kiwi_loft.h).  `flip` MIRRORS THE ROW ORDER, which
        // reverses cross(dCol, dRow) and therefore the surface normal — the
        // two-sided second copy, and the same swap kiwi_patchfillet.cpp:1437-1443
        // performs for its `rowFlip`.
        void WriteCurveStrip( patchMesh_t *p, int e, int n, int segs, bool flip ) const
        {
            for ( int s = 0; s <= segs; ++s )
            {
                const ring_t &st = m_stations[(size_t)s];
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
                        p->ctrl[s * 2][dst].xyz[k] = r0[k] + ( r1[k] - r0[k] ) * f;
                }

                if ( s == segs )
                    continue;

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
                        p->ctrl[s * 2 + 1][dst].xyz[k] = ha[k] + ( hb[k] - ha[k] ) * f;
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
            const int segs = (int)m_stations.size() - 1;
            const int n    = RingN( m_stations[0] );
            const int w    = segs * 2 + 1;
            if ( segs < 1 || n < 3 || w > 15 )
            {
                Sys_Printf( "Loft: the curve bridge needs 1..%i spans over a 3+ gon.\n",
                            KLOFT_MAX_CURVE_SEGS );
                return;
            }

            entity_s      *owner = m_a->owner;
            const brush_t *adef  = m_a->def;

            // Same rule and same reason as kiwi_patchfillet.cpp:1421: everything on
            // selected_brushes at COMMIT is stamped "added by this record", and a
            // SOURCE brush carrying that stamp would be DELETED by the undo.
            Select_Deselect( 1 );
            KiwiCmd_UndoBegin( "loft (curve)" );

            int made = 0;
            for ( int e = 0; e < n; ++e )
            {
                // The material this strip CONTINUES (kiwi_loft.h): the face of brush
                // A adjacent to profile edge e, falling back to the source face when
                // the edge is not on any other face's plane (which happens for the
                // extra vertices SubdivideTo inserted on a long edge — those DO lie
                // on the neighbour's plane, so in practice the fallback is for
                // degenerate geometry only).
                int fi = ( e < (int)m_edgeFace.size() ) ? m_edgeFace[(size_t)e] : -1;
                if ( fi < 0 || fi >= adef->faceCount )
                    fi = m_aFace;
                const face_t *src = &adef->faces[fi];

                // ── ROUND AT, ITEM 3c: TWO-SIDED IS THE SAME STRIP, TWICE ────
                // The second copy has its ROWS mirrored, which reverses
                // cross(dCol, dRow) and so the facing (kiwi_loft.h).  Everything
                // else about it — material, alignment, landing — is identical, so
                // it goes through the same block rather than a second spelling of
                // it, and both copies land inside the one undo bracket.
                const int copies = m_twoSided ? 2 : 1;
                for ( int copy = 0; copy < copies; ++copy )
                {
                    patchMesh_t *p = MakeNewPatch();
                    if ( !p )
                        continue;
                    p->width  = w;
                    p->height = KLOFT_CURVE_ROWS;
                    p->type   = PATCH_BEVEL;  // a swept quadratic, same as the fillet arc

                    p->contents = src->contents;
                    p->flags    = src->toolflags;

                    WriteCurveStrip( p, e, n, segs, copy != 0 );

                    // The material, realized, naturalized at the parent's density and
                    // then CAP-aligned — kiwi_patchfillet.cpp:1502-1554's order and its
                    // reasons, unchanged.  CAP rather than lmap because a loft strip is
                    // the fillet ARC's shape (a swept quadratic), not a flat end cap,
                    // and round AN's directive put CAP on exactly that surface.
                    p->texture  = *(patchMesh_material *)&src->mtldef[0].lyrMtl;
                    p->lightmap = *(patchMesh_material *)&src->mtldef[1].lyrMtl;
                    KiwiMtl_RealizePatch( p );
                    Patch_KiwiFinishNewLike( p, &src->mtldef[0].mat_texDef );
                    Patch_KiwiCapAlign( p );

                    brush_t    *pdef = AddBrushForPatch( p, (entity_s *)owner->def );
                    selbrush_t *inst = Brush_AddToList( pdef, owner );
                    Brush_AddToList2( inst );
                    ++made;
                }
            }

            Sel_Clear( KiwiSel() );
            Sel_RebuildFromLegacy();

            Sys_Printf( "Loft: CURVE bridge — %i patch strip(s) over a %i-gon, %i "
                        "span(s), continuity %s/%s%s.  Patches are RENDER SURFACES: "
                        "they have no collision, so put a caulk brush inside the "
                        "curve if the player has to hit it.\n",
                        made, n, segs, ContName( m_cont[0] ), ContName( m_cont[1] ),
                        m_twoSided ? ", TWO-SIDED (each strip emitted twice)" : "" );
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
                // ROUND AO, ITEM 1(a): the HUD announces the FALLBACK before the
                // click, not after it — the preview is already showing the ruled
                // bridge, so the label has to agree with what is on screen.
                // ROUND AT, ITEM 3: CURVE's own settings belong in the label — the
                // continuity is the difference between a curve and a crease, and
                // two-sided doubles what the commit will create.
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
        // ROUND AO, ITEM 1: `m_tangent` (a bool) became a THREE-valued mode.  Every
        // read of it went through one of the two accessors below, so the station
        // math still asks exactly one question ("is the path curved") and the
        // medium is a separate one ("brushes or patches").
        int         m_bridge     = KLOFT_BRIDGE_RULED;
        int         m_segs       = KLOFT_DEF_SEGS_RULED;
        bool        m_segsTyped  = false;
        float       m_tension    = KLOFT_DEF_TENSION;
        int         m_twist      = 0;      // ROUND AO, ITEM 1(d) — whole-vertex steps
        // ── ROUND AT, ITEM 3 ────────────────────────────────────────────────
        // Continuity per END ([0] = START/face A, [1] = END/face B) and the
        // two-sided emit.  All three are CURVE-mode settings and all three
        // persist (KLOFT_SECTION) — Reset() re-seeds from the store, not from a
        // constant, because Reset() runs on every Begin().
        int         m_cont[2]    = { KLOFT_CONT_G1, KLOFT_CONT_G1 };
        bool        m_twoSided   = false;
        // The two ends' PATH tangents, both pointing forward along the path (A ->
        // B).  Filled by BuildStations from the source faces' plane normals — the
        // same `outA` / `-outB` the Hermite uses — and the whole of the item 3(a)
        // fix: the end stations' tangents are these instead of a one-sided
        // difference that is parallel to the chord.
        float       m_endTan[2][3] = { { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 1.0f } };
        bool        m_haveEndTan = false;
        bool        m_ok         = false;
        bool        m_fellBack   = false;  // ROUND AO, ITEM 1(a) — tangent -> ruled
        const char *m_why        = "no loft yet";

        std::vector<ring_t> m_stations;    // segs + 1 rings, world space, planar
        std::vector<float>  m_planeN;      // 3 floats per station
        std::vector<float>  m_planeD;      // 1 float  per station
        std::vector<int>    m_edgeFace;    // ROUND AO — brush-A face per profile edge
        char                m_hud[192] = { 0 };
    };

    KiwiLoftCommand s_loft;
}

// ─── §3 canExecute ───────────────────────────────────────────────────────────
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

// ─── registration + lookup ───────────────────────────────────────────────────
void KiwiLoft_RegisterCommands()
{
    extern bool Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId );
    // Unbound here — this is the CLASSIC-profile row.  kiwi_keymap.cpp puts it on
    // bare L in the modern profile (Plasticity's own chord, default-keymap.ts:270);
    // the vk 0x4C displacement audit is in kiwi_keymap.h.
    Radiant_RegisterCommand( "KiwiLoft", 0, 0, KIWI_CMD_LOFT );
}

KiwiEditorCommand *KiwiLoft_CommandForId( int commandId )
{
    if ( commandId == KIWI_CMD_LOFT )
        return &s_loft;
    return 0;
}

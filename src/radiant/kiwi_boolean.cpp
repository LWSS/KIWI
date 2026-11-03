#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Difference uses KiwiSplit_DefByPlaneCarve over the ported splitter; union
// delegates to the classic CSG-merge handler.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"                // camera_s
#include <gfx_d3d/r_gfx.h>          // GfxColor
#include <gfx_d3d/r_material.h>     // Material
#include <gfx_d3d/r_rendercmds.h>   // MaterialTechniqueType, TECHNIQUE_UNLIT

#include "kiwi_boolean.h"
#include "kiwi_csg.h"                 // KiwiCsg_BrushUsable
#include "kiwi_boxselect.h"         // KiwiBox_CollectBrushes
#include "kiwi_command.h"
#include "kiwi_construct.h"         // kconPlane_t: a curve tool's cross-section plane
#include "kiwi_extrude.h"           // KiwiExtrude_BuildPrismDef: a curve tool's stand-in
#include "kiwi_lines.h"
#include "kiwi_material.h"          // curve caps: patch material realization
#include "kiwi_patchfillet.h"       // curve caps: KiwiPatch_TextureFromFace
#include "kiwi_pick.h"
#include "kiwi_ported.h"
#include "kiwi_selection.h"
#include "kiwi_split.h"
#include "kiwi_validity.h"          // KiwiValid_Rebuild
#include "kiwi_vec.h"     // Dot3/Sub3/...

#include <algorithm>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <vector>

// Uses the parent face's texture scale instead of the editor default.
extern void Patch_KiwiFinishNewLike( patchMesh_t *p, const texdef_sub_t *srcTex );   // pmesh.cpp

namespace
{
    // mainfrm.cpp:5331 dispatches this id to Cmd_OnSelectionCsgmerge, keeping one
    // merge implementation and undo path.
    const int KBOOL_ID_MERGE = 32927;

    // Difference tools are red; union operands are green to remain distinct from
    // KIWI's blue selection tint. Alpha 0.22 stays visible over lit surfaces.
    const float KBOOL_DIFF_RGBA [4] = { 1.00f, 0.16f, 0.16f, 0.22f };   // the tool, carving
    const float KBOOL_UNION_RGBA[4] = { 0.25f, 1.00f, 0.45f, 0.22f };   // both, combining
    const float KBOOL_DIFF_LINE [3] = { 1.00f, 0.35f, 0.30f };
    const float KBOOL_UNION_LINE[3] = { 0.40f, 1.00f, 0.55f };
    // Shared modelling-layer yellow: this is what the next click will take.
    const float KBOOL_HOT_LINE  [3] = { 1.00f, 0.90f, 0.30f };

    // Fill costs one draw per face. Cap common multi-cut gestures at 32 filled
    // operands, spending newest-first; older operands fall back to the reported line budget.
    // KIWI (2026-09-28): 32 -> 512 - a window grid of cutters is ~80 boxes, and the unfilled ones
    // read as "not picked".
    const int KBOOL_MAX_FILL = 512;

    // Preserve room for the framework's later snap marker and hover colour run.
    const int KBOOL_LINE_HEADROOM = 96;


    // The shared CSG gate rejects patches and fixed-size entities; boolean also
    // requires four planes because fewer cannot bound a volume.
    inline bool Usable( const selbrush_t *b )
    {
        return KiwiCsg_BrushUsable( b ) && b->def->faceCount >= 4;
    }

    // KIWI (2026-10-31): an extruded curve may be a difference tool (curveTool_t below).
    bool CurveAxis( const patchMesh_t *p, bool alongRows, float axis[3] );
    bool CurveCuts( const selbrush_t *b )
    {
        float axis[3];
        const patchMesh_t *p = ( b && b->patch && b->def ) ? b->def->patch : nullptr;
        return p && ( CurveAxis( p, true, axis ) || CurveAxis( p, false, axis ) );
    }

    // Brush_BuildWindings keeps these bounds current; reject misses before N splits.
    bool BoundsOverlap( const brush_t *a, const brush_t *b )
    {
        for ( int k = 0; k < 3; ++k )
            if ( a->mins[k] > b->maxs[k] || a->maxs[k] < b->mins[k] )
                return false;
        return true;
    }

    void FreeDefs( std::vector<brush_t *> *defs )
    {
        for ( size_t i = 0; i < defs->size(); ++i )
            KiwiSplit_FreeUnlandedDef( ( *defs )[i] );
        defs->clear();
    }

    // ── Curve tools ──────────────────────────────────────────────────────────────
    // KIWI (2026-10-31, user: "Make it possible so that a brush can be boolean'd (diff) with a
    // curve (cylinder/etc), and the result is a square box with a curve running perfectly
    // through it, and caps that line up with the brush exterior"): a curve whose every control
    // column is a straight line along one shared axis (cylinders, pipes, arches - extruded
    // curves) cuts with its control hull swept along that axis.  A Bezier surface lies inside
    // its control hull, so no brush face crosses the curve.  The curve itself is kept: each
    // column's end rows slide along the axis onto the brush faces it leaves through, so its
    // ends lie flush on them (sliding along one direction onto a plane is affine, so the end
    // curve lies exactly in the face).
    //
    // KIWI (2026-11-01, user, of an arch cut into a wall: "the brush should probably be a flat
    // square and there should be curved "caps" around the curve"): the hull left the wall's face
    // ragged round the curve (its tangent facets stood off it, open to the void).  A curve whose
    // cross-section is star-shaped about its chord midpoint (open: an arch, never dipping below
    // its chord) or its box centre (closed: a pipe) cuts its bounding RECTANGLE instead, and each
    // wall face it leaves through gets flat cap patches filling rectangle-minus-curve, textured
    // as that face (KiwiPatch_TextureFromFace).  Others keep the hull, uncapped.
    struct capPatch_t                      // cross-section plane space: w columns x 3 rows
    {
        int   w = 0;
        float uv[KPATCH_MAX_WIDTH][3][2];
    };
    struct curveTool_t
    {
        selbrush_t *node      = nullptr;
        brush_t    *prism     = nullptr;   // unlinked stand-in tool, freed after the carve
        float       axis[3]   = { 0.0f, 0.0f, 0.0f };
        bool        alongRows = true;      // ctrl[col][row]: the row index runs along the axis
        std::vector<float> trimmed;        // the new control points, 3 per (around, along)
        bool        changed   = false;     // PlanCurveTrim: some column end moves
        kconPlane_t pl;                    // the cross-section: origin on control (0, 0), normal = axis
        float       lo = 0.0f, hi = 0.0f;  // the curve's extent along the axis from pl.origin
        float       mn2[2] = { 0.0f, 0.0f }, mx2[2] = { 0.0f, 0.0f };   // its controls' bounds, plane space
        std::vector<capPatch_t> caps;      // non-empty: the stand-in is the bounding rectangle
    };

    int CurveAround( const patchMesh_t *p, bool alongRows ) { return alongRows ? p->width  : p->height; }
    int CurveAlong ( const patchMesh_t *p, bool alongRows ) { return alongRows ? p->height : p->width;  }
    float *CurvePt( patchMesh_t *p, bool alongRows, int around, int along )
    {
        return alongRows ? p->ctrl[around][along].xyz : p->ctrl[along][around].xyz;
    }
    const float *CurvePt( const patchMesh_t *p, bool alongRows, int around, int along )
    {
        return alongRows ? p->ctrl[around][along].xyz : p->ctrl[along][around].xyz;
    }

    // Every column a straight line along one shared direction (unit, into `axis`).
    bool CurveAxis( const patchMesh_t *p, bool alongRows, float axis[3] )
    {
        const int na = CurveAround( p, alongRows ), nl = CurveAlong( p, alongRows );
        if ( na < 3 || nl < 2 )
            return false;
        bool have = false;
        for ( int i = 0; i < na; ++i )
        {
            const float *a = CurvePt( p, alongRows, i, 0 );
            float d[3];
            Sub3( CurvePt( p, alongRows, i, nl - 1 ), a, d );
            const float len = Len3( d );
            if ( len < 0.1f )
                return false;
            for ( int k = 0; k < 3; ++k )
                d[k] /= len;
            if ( !have )
            {
                Copy3( d, axis );
                have = true;
            }
            else if ( Dot3( d, axis ) < 0.9995f )
                return false;
            for ( int j = 1; j < nl - 1; ++j )
            {
                float r[3], off[3];
                Sub3( CurvePt( p, alongRows, i, j ), a, r );
                const float t = Dot3( r, d );
                Mad3( r, d, -t, off );
                if ( Len3( off ) > 0.05f || t < -0.05f || t > len + 0.05f )
                    return false;
            }
        }
        return have;
    }

    // Andrew's monotone chain: the CCW hull of 2D points, collinear points dropped.
    void Hull2D( const std::vector<float> &pts, std::vector<float> *out )
    {
        const int n = (int)( pts.size() / 2 );
        std::vector<int> idx( (size_t)n );
        for ( int i = 0; i < n; ++i )
            idx[(size_t)i] = i;
        std::sort( idx.begin(), idx.end(), [&]( int a, int b ) {
            return pts[a * 2] < pts[b * 2] || ( pts[a * 2] == pts[b * 2] && pts[a * 2 + 1] < pts[b * 2 + 1] );
        } );
        auto cross = [&]( int o, int a, int b ) {
            return ( pts[a * 2] - pts[o * 2] ) * ( pts[b * 2 + 1] - pts[o * 2 + 1] )
                 - ( pts[a * 2 + 1] - pts[o * 2 + 1] ) * ( pts[b * 2] - pts[o * 2] );
        };
        std::vector<int> h( (size_t)( 2 * n ) );
        int k = 0;
        for ( int i = 0; i < n; ++i )
        {
            while ( k >= 2 && cross( h[k - 2], h[k - 1], idx[i] ) <= 1.0e-4f ) --k;
            h[k++] = idx[i];
        }
        for ( int i = n - 2, lower = k + 1; i >= 0; --i )
        {
            while ( k >= lower && cross( h[k - 2], h[k - 1], idx[i] ) <= 1.0e-4f ) --k;
            h[k++] = idx[i];
        }
        out->clear();
        for ( int i = 0; i + 1 < k; ++i )      // the last point repeats the first
        {
            out->push_back( pts[h[i] * 2] );
            out->push_back( pts[h[i] * 2 + 1] );
        }
    }

    // The range of a quadratic span projected on one axis, folded into mn / mx.
    void SpanRange( float p0, float p1, float p2, float *mn, float *mx )
    {
        float lo = fminf( p0, p2 ), hi = fmaxf( p0, p2 );
        const float den = p0 - 2.0f * p1 + p2;
        if ( fabsf( den ) > 1.0e-9f )
        {
            const float t = ( p0 - p1 ) / den;
            if ( t > 0.0f && t < 1.0f )
            {
                const float v = ( 1.0f - t ) * ( 1.0f - t ) * p0 + 2.0f * t * ( 1.0f - t ) * p1 + t * t * p2;
                lo = fminf( lo, v );
                hi = fmaxf( hi, v );
            }
        }
        *mn = fminf( *mn, lo );
        *mx = fmaxf( *mx, hi );
    }

    // The bounding rectangle (plane space, CCW, into `box`) and the cap patches filling it round
    // the curve, from its controls `q` (plane space).  Worked in a frame (a along e1, b along e2):
    // the rectangle's boundary is walked from the curve's first point to its last, the way the
    // curve turns.  Each span-end control k gets the boundary point its ray from M hits; each
    // rectangle corner goes to the k whose point is nearest it, so between two consecutive k the
    // boundary is one straight edge - a span maps onto it as a 3-row strip (curve, midway, edge).
    // Corners that share a k become a fan from that control.  False: not this shape.
    bool PlanBoxCaps( const std::vector<float> &q, const kconPlane_t &pl, std::vector<float> *box,
                      std::vector<capPatch_t> *caps )
    {
        const int na = (int)( q.size() / 2 );
        if ( na < 3 || !( na & 1 ) )
            return false;
        const int S = ( na - 1 ) / 2;
        const float *A = &q[0], *B = &q[(size_t)( na - 1 ) * 2];
        const bool closed = hypotf( B[0] - A[0], B[1] - A[1] ) < 0.05f;
        float e1[2], e2[2];
        if ( closed )
        {
            float bu[3], bv[3];                  // level / up across a wall, as bearings read
            KiwiCon_BearingBasis( pl.normal, bu, bv );
            e1[0] = Dot3( bu, pl.u );
            e1[1] = Dot3( bu, pl.v );
        }
        else
        {
            e1[0] = B[0] - A[0];
            e1[1] = B[1] - A[1];
        }
        const float l1 = hypotf( e1[0], e1[1] );
        if ( !( l1 > 1.0e-4f ) )
            return false;
        e1[0] /= l1;
        e1[1] /= l1;
        e2[0] = -e1[1];
        e2[1] = e1[0];
        if ( !closed )
        {
            float c[2] = { 0.0f, 0.0f };         // toward the bulge
            for ( int i = 0; i < na; ++i )
            {
                c[0] += q[(size_t)i * 2] / (float)na;
                c[1] += q[(size_t)i * 2 + 1] / (float)na;
            }
            if ( ( c[0] - A[0] ) * e2[0] + ( c[1] - A[1] ) * e2[1] < 0.0f )
            {
                e2[0] = -e2[0];
                e2[1] = -e2[1];
            }
        }
        std::vector<float> f( q.size() );
        for ( int i = 0; i < na; ++i )
        {
            f[(size_t)i * 2]     = q[(size_t)i * 2] * e1[0] + q[(size_t)i * 2 + 1] * e1[1];
            f[(size_t)i * 2 + 1] = q[(size_t)i * 2] * e2[0] + q[(size_t)i * 2 + 1] * e2[1];
        }
        float mn[2] = { FLT_MAX, FLT_MAX }, mx[2] = { -FLT_MAX, -FLT_MAX };
        for ( int s = 0; s < S; ++s )
            for ( int k = 0; k < 2; ++k )
                SpanRange( f[(size_t)( 4 * s ) + k], f[(size_t)( 4 * s + 2 ) + k], f[(size_t)( 4 * s + 4 ) + k], &mn[k], &mx[k] );
        if ( !closed )
        {
            if ( mn[1] < f[1] - 0.05f )
                return false;                    // dips below its chord (a horseshoe)
            mn[1] = f[1];
        }
        if ( mx[0] - mn[0] < 0.5f || mx[1] - mn[1] < 0.5f )
            return false;
        float M[2];
        if ( closed )
        {
            M[0] = ( mn[0] + mx[0] ) * 0.5f;
            M[1] = ( mn[1] + mx[1] ) * 0.5f;
        }
        else
        {
            M[0] = ( f[0] + f[(size_t)( na - 1 ) * 2] ) * 0.5f;
            M[1] = f[1];
        }

        // Star-shaped about M: every control turns the same way round it.
        int   turn  = 0;
        float sweep = 0.0f;
        float prev  = atan2f( f[1] - M[1], f[0] - M[0] );
        for ( int i = 1; i < na; ++i )
        {
            const float a = atan2f( f[(size_t)i * 2 + 1] - M[1], f[(size_t)i * 2] - M[0] );
            float d = a - prev;
            while ( d > (float)KIWI_PI )   d -= 2.0f * (float)KIWI_PI;
            while ( d <= -(float)KIWI_PI ) d += 2.0f * (float)KIWI_PI;
            const int s = d > 0.0f ? 1 : -1;
            if ( fabsf( d ) < 1.0e-4f || ( turn && s != turn ) )
                return false;
            turn = s;
            sweep += d;
            prev = a;
        }
        if ( fabsf( fabsf( sweep ) - ( closed ? 2.0f : 1.0f ) * (float)KIWI_PI ) > 0.05f )
            return false;

        // The rectangle CCW in the frame, and the boundary path along the curve's turn.
        const float C[4][2] = { { mx[0], mn[1] }, { mx[0], mx[1] }, { mn[0], mx[1] }, { mn[0], mn[1] } };
        auto rayHit = [&]( const float *P, float out[2] ) {
            const float d[2] = { P[0] - M[0], P[1] - M[1] };
            float lam = FLT_MAX;
            for ( int k = 0; k < 2; ++k )
            {
                if ( d[k] > 1.0e-6f )       lam = fminf( lam, ( mx[k] - M[k] ) / d[k] );
                else if ( d[k] < -1.0e-6f ) lam = fminf( lam, ( mn[k] - M[k] ) / d[k] );
            }
            if ( lam == FLT_MAX )
                lam = 0.0f;
            out[0] = M[0] + lam * d[0];
            out[1] = M[1] + lam * d[1];
        };
        std::vector<float> path;
        auto addPt = [&]( const float *pt, bool last ) {
            const size_t n = path.size();
            if ( n >= 2 && hypotf( pt[0] - path[n - 2], pt[1] - path[n - 1] ) < 0.01f )
            {
                if ( last && n > 2 )
                {
                    path[n - 2] = pt[0];
                    path[n - 1] = pt[1];
                }
                return;
            }
            path.push_back( pt[0] );
            path.push_back( pt[1] );
        };
        if ( !closed )
        {
            if ( turn > 0 )
                return false;                    // e2 is the bulge, so A -> B turns clockwise
            addPt( &f[0], false );
            for ( int c = 3; c >= 0; --c )
                addPt( C[c], false );
            addPt( &f[(size_t)( na - 1 ) * 2], true );
        }
        else
        {
            float start[2];
            rayHit( &f[0], start );
            int edge = -1;                       // C[edge] -> C[edge + 1] holds the start
            for ( int i = 0; i < 4 && edge < 0; ++i )
            {
                const float *a = C[i], *b = C[( i + 1 ) & 3];
                const float len = hypotf( b[0] - a[0], b[1] - a[1] );
                const float u = ( ( start[0] - a[0] ) * ( b[0] - a[0] ) + ( start[1] - a[1] ) * ( b[1] - a[1] ) ) / len;
                const float cr = ( ( start[0] - a[0] ) * ( b[1] - a[1] ) - ( start[1] - a[1] ) * ( b[0] - a[0] ) ) / len;
                if ( fabsf( cr ) < 0.01f && u >= -0.01f && u <= len + 0.01f )
                    edge = i;
            }
            if ( edge < 0 )
                return false;
            addPt( start, false );
            for ( int i = 1; i <= 4; ++i )
                addPt( C[turn > 0 ? ( edge + i ) & 3 : ( edge + 1 - i + 4 ) & 3], false );
            addPt( start, true );
        }
        const int nv = (int)( path.size() / 2 );
        std::vector<float> cum( (size_t)nv, 0.0f );
        for ( int j = 1; j < nv; ++j )
            cum[(size_t)j] = cum[(size_t)( j - 1 )]
                           + hypotf( path[(size_t)j * 2] - path[(size_t)( j - 1 ) * 2], path[(size_t)j * 2 + 1] - path[(size_t)( j - 1 ) * 2 + 1] );
        const float L = cum[(size_t)( nv - 1 )];
        auto pathPt = [&]( float t, float out[2] ) {
            int j = 0;
            while ( j + 2 < nv && cum[(size_t)( j + 1 )] < t )
                ++j;
            const float len = cum[(size_t)( j + 1 )] - cum[(size_t)j];
            const float u = len > 0.0f ? fminf( fmaxf( ( t - cum[(size_t)j] ) / len, 0.0f ), 1.0f ) : 0.0f;
            out[0] = path[(size_t)j * 2] + ( path[(size_t)( j + 1 ) * 2] - path[(size_t)j * 2] ) * u;
            out[1] = path[(size_t)j * 2 + 1] + ( path[(size_t)( j + 1 ) * 2 + 1] - path[(size_t)j * 2 + 1] ) * u;
        };
        // The first path position of `pt` at or after tmin (else the nearest).
        auto paramOf = [&]( const float *pt, float tmin ) {
            float first = -1.0f, nearT = 0.0f, nearD = FLT_MAX;
            for ( int j = 0; j + 1 < nv; ++j )
            {
                const float *a = &path[(size_t)j * 2], *b = &path[(size_t)( j + 1 ) * 2];
                const float len = cum[(size_t)( j + 1 )] - cum[(size_t)j];
                if ( !( len > 0.0f ) )
                    continue;
                float u = ( ( pt[0] - a[0] ) * ( b[0] - a[0] ) + ( pt[1] - a[1] ) * ( b[1] - a[1] ) ) / len;
                u = fminf( fmaxf( u, 0.0f ), len );
                const float px = a[0] + ( b[0] - a[0] ) * u / len, py = a[1] + ( b[1] - a[1] ) * u / len;
                const float d = hypotf( pt[0] - px, pt[1] - py ), t = cum[(size_t)j] + u;
                if ( d < 0.01f && t >= tmin - 1.0e-3f && first < 0.0f )
                    first = t;
                if ( d < nearD )
                {
                    nearD = d;
                    nearT = t;
                }
            }
            const float t = first >= 0.0f ? first : nearT;
            return fminf( fmaxf( t, tmin ), L );
        };

        // Span ends onto the boundary, then the corners onto span ends.
        std::vector<float> t( (size_t)( S + 1 ) ), lo( (size_t)( S + 1 ) ), hi( (size_t)( S + 1 ) );
        std::vector<char>  snapped( (size_t)( S + 1 ), 0 );
        t[0] = 0.0f;
        t[(size_t)S] = L;
        for ( int k = 1; k < S; ++k )
        {
            float h[2];
            rayHit( &f[(size_t)k * 4], h );
            t[(size_t)k] = paramOf( h, t[(size_t)( k - 1 )] );
        }
        for ( int k = 0; k <= S; ++k )
            lo[(size_t)k] = hi[(size_t)k] = t[(size_t)k];
        for ( int j = 1; j + 1 < nv; ++j )
        {
            int best = 0;
            for ( int k = 1; k <= S; ++k )
                if ( fabsf( t[(size_t)k] - cum[(size_t)j] ) < fabsf( t[(size_t)best] - cum[(size_t)j] ) )
                    best = k;
            if ( !snapped[(size_t)best] )
                lo[(size_t)best] = cum[(size_t)j];
            hi[(size_t)best] = cum[(size_t)j];
            snapped[(size_t)best] = 1;
        }
        lo[0] = 0.0f;
        hi[(size_t)S] = L;
        if ( snapped[(size_t)S] == 0 )
            lo[(size_t)S] = L;
        for ( int k = 1; k < S; ++k )            // free ends stay on their own straight edge
            if ( !snapped[(size_t)k] )
                lo[(size_t)k] = hi[(size_t)k] = fmaxf( t[(size_t)k], hi[(size_t)( k - 1 )] );
        for ( int k = S - 1; k >= 1; --k )
            if ( !snapped[(size_t)k] )
                lo[(size_t)k] = hi[(size_t)k] = fminf( lo[(size_t)k], lo[(size_t)( k + 1 )] );

        // The patches, in frame coordinates first.
        auto toPlane = [&]( const float *fr, float out[2] ) {
            out[0] = fr[0] * e1[0] + fr[1] * e2[0];
            out[1] = fr[0] * e1[1] + fr[1] * e2[1];
        };
        auto finish = [&]( capPatch_t &cp, float row0[][2], float row2[][2] ) {
            for ( int c = 0; c < cp.w; ++c )
            {
                const float mid[2] = { ( row0[c][0] + row2[c][0] ) * 0.5f, ( row0[c][1] + row2[c][1] ) * 0.5f };
                toPlane( row0[c], cp.uv[c][0] );
                toPlane( mid, cp.uv[c][1] );
                toPlane( row2[c], cp.uv[c][2] );
            }
            caps->push_back( cp );
        };
        caps->clear();
        float row0[KPATCH_MAX_WIDTH][2], row2[KPATCH_MAX_WIDTH][2];
        for ( int k0 = 0; k0 < S; )
        {
            int k1 = k0 + 1;                     // the run ends where a fan sits
            while ( k1 < S && hi[(size_t)k1] - lo[(size_t)k1] < 1.0e-4f )
                ++k1;
            capPatch_t cp;
            cp.w = 2 * ( k1 - k0 ) + 1;
            for ( int c = 0; c < cp.w; ++c )
            {
                row0[c][0] = f[(size_t)( 2 * k0 + c ) * 2];
                row0[c][1] = f[(size_t)( 2 * k0 + c ) * 2 + 1];
                if ( !( c & 1 ) )
                {
                    const int k = k0 + c / 2;
                    pathPt( k == k0 ? hi[(size_t)k] : lo[(size_t)k], row2[c] );
                }
            }
            for ( int c = 1; c < cp.w; c += 2 )
            {
                row2[c][0] = ( row2[c - 1][0] + row2[c + 1][0] ) * 0.5f;
                row2[c][1] = ( row2[c - 1][1] + row2[c + 1][1] ) * 0.5f;
            }
            finish( cp, row0, row2 );
            k0 = k1;
        }
        for ( int k = 0; k <= S; ++k )
        {
            if ( hi[(size_t)k] - lo[(size_t)k] < 1.0e-4f )
                continue;
            float vtx[8][2];
            int nvtx = 0;
            pathPt( lo[(size_t)k], vtx[nvtx++] );
            for ( int j = 1; j + 1 < nv; ++j )
                if ( cum[(size_t)j] > lo[(size_t)k] + 1.0e-4f && cum[(size_t)j] < hi[(size_t)k] - 1.0e-4f )
                {
                    vtx[nvtx][0] = path[(size_t)j * 2];
                    vtx[nvtx][1] = path[(size_t)j * 2 + 1];
                    ++nvtx;
                }
            pathPt( hi[(size_t)k], vtx[nvtx++] );
            const float *apex = &f[(size_t)k * 4];
            float area = 0.0f;
            for ( int v = 0; v + 1 < nvtx; ++v )
                area += ( vtx[v][0] - apex[0] ) * ( vtx[v + 1][1] - apex[1] ) - ( vtx[v][1] - apex[1] ) * ( vtx[v + 1][0] - apex[0] );
            if ( fabsf( area ) * 0.5f < 0.5f )
                continue;                        // the fan lies along the boundary: nothing to fill
            capPatch_t cp;
            cp.w = 2 * ( nvtx - 1 ) + 1;
            for ( int c = 0; c < cp.w; ++c )
            {
                row0[c][0] = apex[0];
                row0[c][1] = apex[1];
                if ( c & 1 )
                {
                    row2[c][0] = ( vtx[c / 2][0] + vtx[c / 2 + 1][0] ) * 0.5f;
                    row2[c][1] = ( vtx[c / 2][1] + vtx[c / 2 + 1][1] ) * 0.5f;
                }
                else
                {
                    row2[c][0] = vtx[c / 2][0];
                    row2[c][1] = vtx[c / 2][1];
                }
            }
            finish( cp, row0, row2 );
        }

        box->clear();
        const bool ccw = e1[0] * e2[1] - e1[1] * e2[0] > 0.0f;
        for ( int i = 0; i < 4; ++i )
        {
            float p2[2];
            toPlane( C[ccw ? i : 3 - i], p2 );
            box->push_back( p2[0] );
            box->push_back( p2[1] );
        }
        return true;
    }

    // The curve's stand-in tool: its bounding rectangle (PlanBoxCaps) or else its control hull,
    // swept along its axis over its own length.
    bool CurveToolPrism( selbrush_t *node, curveTool_t *out, const char **why )
    {
        const patchMesh_t *p = ( node && node->def ) ? node->def->patch : nullptr;
        if ( !p )
        {
            *why = "it is not a curve";
            return false;
        }
        bool rows = true;
        if ( !CurveAxis( p, true, out->axis ) )
        {
            rows = false;
            if ( !CurveAxis( p, false, out->axis ) )
            {
                *why = "it is not straight along one axis (extruded curves - cylinders, pipes, "
                       "arches - can cut)";
                return false;
            }
        }
        out->node      = node;
        out->alongRows = rows;

        kconPlane_t pl;
        if ( !KiwiCon_MakePlane( CurvePt( p, rows, 0, 0 ), out->axis, nullptr, &pl ) )
        {
            *why = "its axis is degenerate";
            return false;
        }
        const int na = CurveAround( p, rows ), nl = CurveAlong( p, rows );
        std::vector<float> uv;
        float lo = FLT_MAX, hi = -FLT_MAX;
        for ( int i = 0; i < na; ++i )
            for ( int j = 0; j < nl; ++j )
            {
                const float *w = CurvePt( p, rows, i, j );
                float r[3];
                Sub3( w, pl.origin, r );
                const float t = Dot3( r, pl.normal );
                lo = ( t < lo ) ? t : lo;
                hi = ( t > hi ) ? t : hi;
                if ( j == 0 )
                {
                    float q[2];
                    KiwiCon_WorldToPlane( pl, w, q );
                    uv.push_back( q[0] );
                    uv.push_back( q[1] );
                }
            }
        out->pl = pl;
        out->lo = lo;
        out->hi = hi;
        for ( int k = 0; k < 2; ++k )
        {
            out->mn2[k] = FLT_MAX;
            out->mx2[k] = -FLT_MAX;
        }
        for ( size_t i = 0; i + 1 < uv.size(); i += 2 )
            for ( int k = 0; k < 2; ++k )
            {
                out->mn2[k] = fminf( out->mn2[k], uv[i + k] );
                out->mx2[k] = fmaxf( out->mx2[k], uv[i + k] );
            }
        std::vector<float> hull;
        if ( !PlanBoxCaps( uv, pl, &hull, &out->caps ) )
        {
            out->caps.clear();
            Hull2D( uv, &hull );
        }
        const int n = (int)( hull.size() / 2 );
        float area = 0.0f;
        for ( int i = 0; i < n; ++i )
        {
            const int j = ( i + 1 ) % n;
            area += hull[i * 2] * hull[j * 2 + 1] - hull[j * 2] * hull[i * 2 + 1];
        }
        if ( n < 3 || area * 0.5f < 1.0f )
        {
            *why = "it has no cross-section to cut with (a flat sheet)";
            return false;
        }
        if ( n > KEXT_MAX_PROFILE )
        {
            *why = "its cross-section has too many corners";
            return false;
        }
        out->prism = KiwiExtrude_BuildPrismDef( pl, &hull[0], n, lo, hi, why );
        if ( !out->prism )
            return false;
        KiwiValid_Rebuild( out->prism );
        return true;
    }

    // Where the line p + s*axis is inside the convex brush; false when it never is.
    bool ClipLine( const brush_t *def, const float p[3], const float axis[3], float *sIn, float *sOut,
                   int *faceIn = nullptr, int *faceOut = nullptr )
    {
        float in = -FLT_MAX, out = FLT_MAX;
        int   fIn = -1, fOut = -1;
        for ( int f = 0; f < def->faceCount; ++f )
        {
            const float *n   = def->faces[f].plane.normal;
            const float  den = Dot3( n, axis );
            const float  num = def->faces[f].plane.dist - Dot3( n, p );
            if ( fabsf( den ) < 1.0e-6f )
            {
                if ( num < 0.0f )
                    return false;              // parallel and outside this face
                continue;
            }
            const float t = num / den;
            if ( den > 0.0f && t < out )
            {
                out  = t;
                fOut = f;
            }
            else if ( den < 0.0f && t > in )
            {
                in  = t;
                fIn = f;
            }
        }
        if ( !( in < out ) )
            return false;
        *sIn  = in;
        *sOut = out;
        if ( faceIn )
            *faceIn = fIn;
        if ( faceOut )
            *faceOut = fOut;
        return true;
    }

    // The curve's control points with every column's ends trimmed to where it leaves the
    // brushes (`changed`: some end moves); false - a column passing beside them all - leaves the
    // curve alone and uncapped.
    bool PlanCurveTrim( curveTool_t *ct, const std::vector<const brush_t *> &brushes )
    {
        const patchMesh_t *p = ct->node->def->patch;
        const bool rows = ct->alongRows;
        const int  na = CurveAround( p, rows ), nl = CurveAlong( p, rows );
        ct->trimmed.clear();
        bool &changed = ct->changed;
        changed = false;
        for ( int i = 0; i < na; ++i )
        {
            const float *a = CurvePt( p, rows, i, 0 );
            float r[3];
            Sub3( CurvePt( p, rows, i, nl - 1 ), a, r );
            const float len = Dot3( r, ct->axis );
            float sIn = FLT_MAX, sOut = -FLT_MAX;
            bool  hit = false;
            for ( const brush_t *b : brushes )
            {
                float i0, o0;
                if ( !ClipLine( b, a, ct->axis, &i0, &o0 ) )
                    continue;
                hit  = true;
                sIn  = ( i0 < sIn ) ? i0 : sIn;
                sOut = ( o0 > sOut ) ? o0 : sOut;
            }
            if ( !hit )
                return false;
            const float s0 = ( sIn  > 0.0f ) ? sIn  : 0.0f;
            const float s1 = ( sOut < len  ) ? sOut : len;
            if ( s1 - s0 < 0.1f )
                return false;
            if ( s0 > 1.0e-3f || s1 < len - 1.0e-3f )
                changed = true;
            for ( int j = 0; j < nl; ++j )
            {
                Sub3( CurvePt( p, rows, i, j ), a, r );
                const float f = ( len > 0.0f ) ? Dot3( r, ct->axis ) / len : 0.0f;
                float w[3];
                Mad3( a, ct->axis, s0 + ( s1 - s0 ) * f, w );
                ct->trimmed.insert( ct->trimmed.end(), w, w + 3 );
            }
        }
        return true;
    }

    // KIWI (2026-11-01, user: "the ceiling curve is making the faces backwards. The backwards
    // faces should be the ones going into the brushes"): a kept curve lines the hole it cut, so it
    // faces into its own cross-section - the opening - and its back goes into the wall.  Extrude
    // builds an arch facing out of its solid, the other way.  True: it faces out (flip it).
    bool CurveFacesWall( const curveTool_t &ct )
    {
        const patchMesh_t *p = ct.node->def->patch;
        const int na = CurveAround( p, ct.alongRows );
        // The cross-section's area centroid (the controls closed by their chord).
        std::vector<float> q;
        for ( int i = 0; i < na; ++i )
        {
            float uv[2];
            KiwiCon_WorldToPlane( ct.pl, CurvePt( p, ct.alongRows, i, 0 ), uv );
            q.push_back( uv[0] );
            q.push_back( uv[1] );
        }
        float a = 0.0f, cx = 0.0f, cy = 0.0f, mx = 0.0f, my = 0.0f;
        for ( int i = 0; i < na; ++i )
        {
            const int j = ( i + 1 ) % na;
            const float cr = q[(size_t)i * 2] * q[(size_t)j * 2 + 1] - q[(size_t)j * 2] * q[(size_t)i * 2 + 1];
            a  += cr;
            cx += ( q[(size_t)i * 2] + q[(size_t)j * 2] ) * cr;
            cy += ( q[(size_t)i * 2 + 1] + q[(size_t)j * 2 + 1] ) * cr;
            mx += q[(size_t)i * 2] / (float)na;
            my += q[(size_t)i * 2 + 1] / (float)na;
        }
        if ( fabsf( a ) > 1.0e-3f )
        {
            mx = cx / ( 3.0f * a );
            my = cy / ( 3.0f * a );
        }
        // The surface normal, cross(dCol, dRow), at the middle of the control net.
        const int c = p->width / 2, r = p->height / 2;
        float dCol[3], dRow[3], n[3], pt[2], n2[2];
        Sub3( p->ctrl[c + 1 < p->width ? c + 1 : c][r].xyz, p->ctrl[c > 0 ? c - 1 : c][r].xyz, dCol );
        Sub3( p->ctrl[c][p->height - 1].xyz, p->ctrl[c][0].xyz, dRow );
        Cross3( dCol, dRow, n );
        KiwiCon_WorldToPlane( ct.pl, p->ctrl[c][r].xyz, pt );
        n2[0] = Dot3( n, ct.pl.u );
        n2[1] = Dot3( n, ct.pl.v );
        return n2[0] * ( mx - pt[0] ) + n2[1] * ( my - pt[1] ) < 0.0f;
    }

    // The planned caps on each wall face the curve leaves through: every point slides along the
    // axis onto where its line enters (end 0) / leaves (end 1) the cut brushes, as the curve's
    // ends do.  An end whose wall face lies beyond the curve (a pocket, closed by the stand-in's
    // own end face) or whose points miss the brushes gets none.  Landed in `owners[brush]`, each
    // patch textured as the face it lies on.  Returns the count landed.
    int LandCurveCaps( const curveTool_t &ct, const std::vector<const brush_t *> &brushes,
                       const std::vector<entity_s *> &owners )
    {
        const patchMesh_t *curve = ct.node->def->patch;
        int landed = 0;
        for ( int end = 0; end < 2; ++end )
        {
            std::vector<float> xyz;              // per cap, per (col, row), 3 floats
            int srcBrush = -1, srcFace = -1;
            bool ok = true;
            for ( size_t c = 0; c < ct.caps.size() && ok; ++c )
                for ( int col = 0; col < ct.caps[c].w && ok; ++col )
                    for ( int row = 0; row < 3 && ok; ++row )
                    {
                        float base[3];
                        KiwiCon_PlaneToWorld( ct.pl, ct.caps[c].uv[col][row], base );
                        float s = end ? -FLT_MAX : FLT_MAX;
                        int   sb = -1, sf = -1;
                        for ( size_t b = 0; b < brushes.size(); ++b )
                        {
                            float i0, o0;
                            int   fi, fo;
                            if ( !ClipLine( brushes[b], base, ct.axis, &i0, &o0, &fi, &fo ) )
                                continue;
                            if ( !end && i0 < s ) { s = i0; sb = (int)b; sf = fi; }
                            if ( end && o0 > s )  { s = o0; sb = (int)b; sf = fo; }
                        }
                        if ( sb < 0 || sf < 0 )
                            ok = false;
                        else if ( !end && s < ct.lo - 0.05f )
                            ok = false;
                        else if ( end && s > ct.hi + 0.05f )
                            ok = false;
                        else
                        {
                            s = end ? fminf( s, ct.hi ) : fmaxf( s, ct.lo );
                            float w[3];
                            Mad3( base, ct.axis, s, w );
                            xyz.insert( xyz.end(), w, w + 3 );
                            if ( srcBrush < 0 )
                            {
                                srcBrush = sb;
                                srcFace  = sf;
                            }
                        }
                    }
            if ( !ok || srcBrush < 0 )
                continue;
            const face_t &src = brushes[(size_t)srcBrush]->faces[srcFace];
            entity_s     *owner = owners[(size_t)srcBrush];
            float want[3];
            Copy3( ct.axis, want );
            if ( !end )
                for ( int k = 0; k < 3; ++k )
                    want[k] = -want[k];
            size_t at = 0;
            for ( size_t c = 0; c < ct.caps.size(); ++c )
            {
                patchMesh_t *p = MakeNewPatch();
                if ( !p )
                    return landed;
                p->width      = ct.caps[c].w;
                p->height     = 3;
                p->type       = (PATCH_TYPES)( PATCH_BEVEL | PATCH_SEAM );
                p->contents   = src.contents;
                p->flags      = src.toolflags;     // MakeNewPatch does NOT seed this
                p->subDivType = curve->subDivType;
                for ( int col = 0; col < p->width; ++col )
                    for ( int row = 0; row < 3; ++row, at += 3 )
                        Copy3( &xyz[at], p->ctrl[col][row].xyz );
                // Facing out of the wall: cross(dCol, dRow) summed over the net.
                float n[3] = { 0.0f, 0.0f, 0.0f };
                for ( int col = 0; col + 1 < p->width; ++col )
                    for ( int row = 0; row < 2; ++row )
                    {
                        float a[3], b[3], x[3];
                        Sub3( p->ctrl[col + 1][row].xyz, p->ctrl[col][row].xyz, a );
                        Sub3( p->ctrl[col][row + 1].xyz, p->ctrl[col][row].xyz, b );
                        Cross3( a, b, x );
                        Add3( n, x, n );
                    }
                if ( Dot3( n, want ) < 0.0f )
                    for ( int col = 0; col < p->width; ++col )
                    {
                        const drawVert_t v = p->ctrl[col][0];
                        p->ctrl[col][0] = p->ctrl[col][2];
                        p->ctrl[col][2] = v;
                    }
                p->texture  = *(patchMesh_material *)&src.mtldef[0].lyrMtl;
                p->lightmap = *(patchMesh_material *)&src.mtldef[1].lyrMtl;
                KiwiMtl_RealizePatch( p );
                KiwiMtl_EnsurePatchChannels( p );
                Patch_KiwiFinishNewLike( p, &src.mtldef[0].mat_texDef );
                KiwiPatch_TextureFromFace( p, &src );

                brush_t    *pdef = AddBrushForPatch( p, (entity_s *)owner->def );
                selbrush_t *inst = Brush_AddToList( pdef, owner );
                Brush_AddToList2( inst );
                ++landed;
            }
        }
        return landed;
    }

    // N-plane subtract; see kiwi_boolean.h.
    enum carveResult_t
    {
        KBOOL_CARVED = 0,   // outPieces holds the surviving parts, none landed yet
        KBOOL_MISS,         // the two solids do not intersect: nothing to do, no error
        KBOOL_CONSUMED,     // the input lies wholly INSIDE the tool: zero pieces survive
        KBOOL_REFUSED       // §19 said no
    };

    // `target` is borrowed and never mutated; all outputs and remainders are fresh,
    // unlanded clones. Splitter material rules therefore texture new cut faces from
    // the target descendant, never from the disposable tool.
    //
    // A front half may be landed, so §19 rejects or drops invalid fronts. The back
    // half is only a running intersection and is discarded; a collapsed back means
    // a miss, while a refused back with real extent may be carried and reported.
    // KBOOL_CONSUMED is only a per-piece result; CarveTarget decides whether the
    // user's original target disappeared and must instead be refused.
    int s_carveSliverDrops = 0;

    // Per-commit diagnostics: skipped cuts, first miss reason, and unlanded backs
    // carried past §19. The command is synchronous and resets these before carving.
    // Emit at most eight per-cut messages while retaining the full refusal count.
    enum { KBOOL_REFUSE_REPORT_MAX = 8 };
    int  s_carveRefusals   = 0;
    char s_carveMissWhy[192] = { 0 };
    int  s_carveCarried    = 0;

    void KiwiBool_ResetCarveReport()
    {
        s_carveSliverDrops = 0;
        s_carveRefusals    = 0;
        s_carveCarried     = 0;              // reset per commit
        s_carveMissWhy[0]  = '\0';
    }

    carveResult_t CarveByOneTool( brush_t *target, const brush_t *tool,
                                  std::vector<brush_t *> *outPieces, const char **why )
    {
        outPieces->clear();
        *why = "unknown";

        if ( !BoundsOverlap( target, tool ) )
        {
            *why = "their bounding boxes do not overlap";   // cheap miss
            return KBOOL_MISS;
        }

        brush_t *remainder = target;
        bool     ownRemainder = false;      // true once `remainder` is ours to free

        for ( int f = 0; f < tool->faceCount; ++f )
        {
            const face_t &tf = tool->faces[f];

            brush_t        *front = 0, *back = 0;
            kiwiSplitHalf_t fs = KSPLIT_HALF_NONE, bs = KSPLIT_HALF_NONE;
            // The back is an unlanded running intersection. The splitter may carry
            // a §19-refused back only when every extent meets KSPLIT_CARRY_EXTENT;
            // front halves that could become brushes remain fully gated.
            bool backRefused = false;
            if ( !KiwiSplit_DefByPlaneCarve( remainder, tf.planepts[0], tf.planepts[1],
                                             tf.planepts[2], &front, &back,
                                             &fs, &bs, &backRefused, why ) )
            {
                // The splitter's false contract allocates nothing.
                FreeDefs( outPieces );
                if ( ownRemainder )
                    KiwiSplit_FreeUnlandedDef( remainder );
                return KBOOL_REFUSED;
            }

            if ( bs != KSPLIT_HALF_OK )
            {
                // NONE puts the remainder wholly outside this tool plane; SLIVER
                // collapses the running intersection below §19. Both mean no
                // removable shared volume, so leave the target whole.
                if ( front )
                    KiwiSplit_FreeUnlandedDef( front );
                FreeDefs( outPieces );
                if ( ownRemainder )
                    KiwiSplit_FreeUnlandedDef( remainder );
                // Distinguish a true miss from a near-tangent collapse. Copy first
                // because `*why` may alias the static destination used below.
                char gateWhy[96];
                _snprintf( gateWhy, sizeof( gateWhy ), "%s", *why ? *why : "invalid geometry" );
                gateWhy[sizeof( gateWhy ) - 1] = '\0';
                static char s_missWhy[160];
                _snprintf( s_missWhy, sizeof( s_missWhy ),
                           ( bs == KSPLIT_HALF_NONE )
                             ? "no shared volume — tool plane %i leaves the target "
                               "entirely outside it"
                             : "the shared volume collapsed below the validity gate "
                               "at tool plane %i (a near-tangent cut: %s)",
                           f, gateWhy );
                s_missWhy[sizeof( s_missWhy ) - 1] = '\0';
                *why = s_missWhy;
                return KBOOL_MISS;
            }

            // Carried intermediates never land, but report available gate detail because the
            // resulting hole may be inexact around this plane.
            if ( backRefused )
            {
                ++s_carveCarried;
                if ( s_carveCarried <= KBOOL_REFUSE_REPORT_MAX )
                    Sys_Printf( "Boolean: tool plane %i left the running intersection "
                                "outside the validity gate (%s) — carried on, because "
                                "that piece is an intermediate and is never landed.\n",
                                f, ( *why && **why ) ? *why : "invalid geometry" );
            }

            if ( fs == KSPLIT_HALF_NONE )
            {
                // Everything is INSIDE this half-space: this plane carves nothing.
                // `back` is a re-clone of the same volume, so drop it and carry the
                // remainder we already have to the next plane.
                KiwiSplit_FreeUnlandedDef( back );
                continue;
            }

            if ( fs == KSPLIT_HALF_SLIVER )
            {
                // The outside slice is below the §19 gate: it was freed by the
                // splitter and it is NOT a piece.  The remainder still advances —
                // the volume forfeited is smaller than the gate that rejected it.
                ++s_carveSliverDrops;
            }
            else
            {
                outPieces->push_back( front );      // outside this plane = it survives
            }

            if ( ownRemainder )
                KiwiSplit_FreeUnlandedDef( remainder );
            remainder    = back;                    // still possibly inside the tool
            ownRemainder = true;
        }

        // The final remainder is target ∩ tool; discard it without landing it.
        if ( ownRemainder )
            KiwiSplit_FreeUnlandedDef( remainder );

        if ( outPieces->empty() )
        {
            // The input lies wholly inside this tool; the cascade owns the verdict.
            return KBOOL_CONSUMED;
        }
        return KBOOL_CARVED;
    }

    // Cascade each tool over every surviving piece to compute
    // `target - (t0 ∪ t1 ∪ …)` without constructing a non-convex tool union.
    // `!owned` means cur contains only the borrowed target; once any cut changes it,
    // every entry is an owned unlanded clone. Misses and refused individual cuts
    // preserve their input piece and allow the rest of the cascade to continue.
    carveResult_t CarveTarget( brush_t *target, brush_t *const *tools, int toolCount,
                               std::vector<brush_t *> *outPieces, const char **why )
    {
        outPieces->clear();
        *why = "unknown";
        if ( !target || !tools || toolCount <= 0 )
            return KBOOL_MISS;

        std::vector<brush_t *> cur;
        bool                   owned = false;      // see the OWNERSHIP note above
        cur.push_back( target );

        for ( int t = 0; t < toolCount; ++t )
        {
            const brush_t *tool = tools[t];
            if ( !tool )
                continue;

            std::vector<brush_t *> next;
            bool                   changed = false;

            for ( size_t p = 0; p < cur.size(); ++p )
            {
                std::vector<brush_t *> made;
                const carveResult_t r = CarveByOneTool( cur[p], tool, &made, why );

                if ( r == KBOOL_REFUSED )
                {
                    // A refusal frees all attempted outputs and leaves the input
                    // piece unchanged, so skip only this cut. Nothing invalid lands;
                    // report the possibly incomplete hole, with a bounded line count.
                    ++s_carveRefusals;
                    if ( s_carveRefusals <= KBOOL_REFUSE_REPORT_MAX )
                        Sys_Printf( "Boolean: tool %i could not cut piece %i — %s.  "
                                    "That cut was skipped; the rest of the "
                                    "difference still ran.\n",
                                    t, (int)p, ( *why && **why ) ? *why : "invalid geometry" );
                    else if ( s_carveRefusals == KBOOL_REFUSE_REPORT_MAX + 1 )
                        Sys_Printf( "Boolean: (further per-cut refusals suppressed "
                                    "for this operation)\n" );
                    // KIWI: failure hint - where this tool meets the piece it could not cut.
                    if ( !KiwiCsg_FailHintAddOverlap( cur[p], tool ) )
                        KiwiCsg_FailHintAddBrush( cur[p] );
                    next.push_back( cur[p] );      // survives whole; ownership unchanged
                    continue;
                }
                if ( r == KBOOL_MISS )
                {
                    // Latch the first miss observed across this commit.
                    if ( !s_carveMissWhy[0] && *why && **why )
                    {
                        _snprintf( s_carveMissWhy, sizeof( s_carveMissWhy ),
                                   "tool %i vs piece %i: %s", t, (int)p, *why );
                        s_carveMissWhy[sizeof( s_carveMissWhy ) - 1] = '\0';
                    }
                    next.push_back( cur[p] );      // survives whole; ownership unchanged
                    continue;
                }

                // CARVED or CONSUMED replaces this piece with `made`. The first
                // input is the borrowed map target; later inputs are owned clones.
                changed = true;
                if ( owned )
                    KiwiSplit_FreeUnlandedDef( cur[p] );
                for ( size_t k = 0; k < made.size(); ++k )
                    next.push_back( made[k] );
            }

            cur.swap( next );
            if ( changed )
                owned = true;
        }

        if ( !owned )
        {
            // Every tool missed; cur is still the borrowed target. Return the
            // commit-wide first reason so the caller can distinguish miss classes.
            if ( s_carveMissWhy[0] )
                *why = s_carveMissWhy;
            return KBOOL_MISS;
        }
        if ( cur.empty() )
        {
            // Refuse an empty result rather than silently deleting the target.
            *why = "it is entirely inside the tool(s) (a difference would remove it "
                   "completely) — delete it directly if that is what you want";
            return KBOOL_REFUSED;
        }
        outPieces->swap( cur );
        return KBOOL_CARVED;
    }

    // Convex face windings are triangle fans. Bracket per-vertex colour with the
    // selected-face fill's neutral/white material state (camwnd.cpp 0x408106).
    const int KBOOL_FILL_MAX_PTS = 64;      // fixed preview scratch; larger legal faces are skipped

    void FillBrush( const brush_t *def, const float rgba[4] )
    {
        if ( !def || !def->faces )
            return;
        // Ed_Camera never returns NULL (camwnd.cpp:159).
        const camera_s *cam = Ed_Camera();

        static const float s_neutral[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        static const float s_white[4]   = { 1.0f, 1.0f, 1.0f, 1.0f };

        float rgbaCopy[4] = { rgba[0], rgba[1], rgba[2], rgba[3] };
        GfxColor packed;
        Byte4PackPixelColor( rgbaCopy, &packed );
        const float packedAsFloat = *(float *)&packed.packed;   // bit-cast, as the ported batcher does

        static float    s_xyzw  [KBOOL_FILL_MAX_PTS][4];
        static float    s_normal[KBOOL_FILL_MAX_PTS][3];
        static float    s_st    [KBOOL_FILL_MAX_PTS][2];
        static float    s_color [KBOOL_FILL_MAX_PTS];
        static uint16_t s_idx   [( KBOOL_FILL_MAX_PTS - 2 ) * 3];

        R_AddCmdSetMaterialColor( s_neutral );
        for ( int f = 0; f < def->faceCount; ++f )
        {
            const winding_t *w = def->faces[f].w;
            if ( !w || w->numpoints < 3 || w->numpoints > KBOOL_FILL_MAX_PTS )
                continue;
            const int n = w->numpoints;
            for ( int i = 0; i < n; ++i )
            {
                s_xyzw[i][0] = w->p[i][0];
                s_xyzw[i][1] = w->p[i][1];
                s_xyzw[i][2] = w->p[i][2];
                s_xyzw[i][3] = 1.0f;
                Copy3( def->faces[f].plane.normal, s_normal[i] );
                s_st[i][0] = 0.0f;
                s_st[i][1] = 0.0f;
                s_color[i] = packedAsFloat;
            }
            const int tris = n - 2;
            for ( int t = 0; t < tris; ++t )
            {
                s_idx[t * 3 + 0] = 0;
                s_idx[t * 3 + 1] = (uint16_t)( t + 1 );
                s_idx[t * 3 + 2] = (uint16_t)( t + 2 );
            }
            // Orient each outward-wound fan to the eye or the preview becomes a half-shell.
            KiwiTris_OrientToEye( &s_xyzw[0][0], 4, s_idx, tris * 3, cam->origin );
            R_AddRenderCmdDrawTris( g_qeglobals.d_white, TECHNIQUE_UNLIT,
                                    (short)( tris * 3 ), s_idx, (short)n,
                                    s_xyzw, s_normal, s_color, s_st );
        }
        R_AddCmdSetMaterialColor( s_white );
    }

    // Add face boundaries to the framework's shared line batch. Callers reserve
    // exact cost first; a false return remains the no-wrap safety net.
    bool OutlineBrush( const brush_t *def )
    {
        if ( !def || !def->faces )
            return true;
        for ( int f = 0; f < def->faceCount; ++f )
        {
            const winding_t *w = def->faces[f].w;
            if ( !w || w->numpoints < 2 || w->numpoints > MAX_POINTS_ON_WINDING )
                continue;
            for ( int p = 0; p < w->numpoints; ++p )
                if ( !KiwiLines_Add( w->p[p], w->p[( p + 1 ) % w->numpoints] ) )
                    return false;
        }
        return true;
    }

    // Mirror OutlineBrush's walk exactly; winding shapes make estimates unreliable.
    int OutlineCost( const brush_t *def )
    {
        if ( !def || !def->faces )
            return 0;
        int n = 0;
        for ( int f = 0; f < def->faceCount; ++f )
        {
            const winding_t *w = def->faces[f].w;
            if ( !w || w->numpoints < 2 || w->numpoints > MAX_POINTS_ON_WINDING )
                continue;
            n += w->numpoints;
        }
        return n;
    }

    // Q: selected solids are targets; clicked or marquee-selected solids are tools.
    class KiwiBooleanCommand : public KiwiEditorCommand
    {
    public:
        const char *Name() const override { return "Boolean"; }
        // Picks brushes, places nothing: no snap marker / label / dots (KIWI 2026-09-28).
        bool ShowsSnap() const override { return false; }
        bool CanExecute() override { return KiwiBool_CanBoolean(); }

        // Clicks keep adding or replacing tools after the preview goes live;
        // RMB/Enter commits and Esc walks the tool stack back.
        bool WantsClicks() const override { return true; }

        // Shift+drag adds tools in either stage.
        bool WantsMarquee() const override { return true; }

        // This command has no numeric fields.
        int NumericFields( const kiwiNumField_t **out ) const override
        { (void)out; return 0; }

        const char *HudStatus() const override { return m_hud[0] ? m_hud : 0; }

        // The tool must never be picked out of the geometry the verb is about to
        // carve — that is what makes "click ANOTHER solid" unambiguous.
        unsigned PickFlags() const override { return PICKF_EXCLUDE_SELECTED; }

        int HudPrompts( const kiwiPrompt_t **out ) const override
        {
            // Advertise the additive grammar and live-stage Esc behavior explicitly.
            static const kiwiPrompt_t s_pick[] = {
                { "LMB",       "Pick the other solid" },
                { "Shift+LMB", "Add another solid" },
                { "Shift+Drag","Box-add solids" },
            };
            static const kiwiPrompt_t s_live[] = {
                { "Q",         "Difference / Union" },
                { "Shift+LMB", "Add another solid" },
                { "Shift+Drag","Box-add solids" },
                { "Esc",       "Drop the last solid" },
            };
            if ( m_stage == KBOOL_PICK_TOOL )
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

            // Patches can enter selected_brushes in face mode but have no volume;
            // report each skipped class rather than silently dropping the selection.
            int skippedPatches = 0;
            for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
            {
                if ( Usable( b ) )
                    m_targets.push_back( b );
                else if ( b->patch )
                    ++skippedPatches;
            }
            if ( skippedPatches > 0 )
                Sys_Printf( "Boolean: %i curve/patch(es) in the selection were "
                            "skipped — a patch is a render surface with no volume, "
                            "so it can neither be carved nor carve.\n",
                            skippedPatches );
            if ( m_targets.empty() )
            {
                Sys_Printf( "Boolean: select at least one solid first (patches and "
                            "fixed-size entities cannot be booleaned).\n" );
                return false;
            }

            m_stage = KBOOL_PICK_TOOL;
            UpdateHud();
            Sys_Printf( "Boolean: click the OTHER solid to use as the tool — "
                        "Shift+click or Shift+drag a box to add more tools "
                        "(Esc cancels).\n" );
            return true;
        }

        // Recast the framework pick in object mode so component selection modes
        // cannot hide solids. Keep hover live in both stages, admit existing tools
        // for Shift+click removal, and always exclude targets.
        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick; (void)snap;

            selbrush_t *was = m_hover;
            m_hover = 0;
            m_hoverIsPatch = false;             // recomputed from this ray

            int   x, y;
            ray_t ray;
            if ( KiwiCmd_LastCursor( &x, &y ) && Pick_RayFromImagePos( x, y, &ray ) )
            {
                const pick_result_t hit = Pick( ray, SEL_MASK_OBJECT, PICKF_EXCLUDE_SELECTED );
                if ( hit.valid && hit.item.brush && Sel_BrushLive( hit.item.brush )
                  && ( Usable( hit.item.brush ) || CurveCuts( hit.item.brush ) )
                  && !IsTarget( hit.item.brush ) )
                    m_hover = hit.item.brush;
                // Preserve a patch-specific refusal instead of conflating it with empty space.
                else if ( hit.valid && hit.item.brush && Sel_BrushLive( hit.item.brush )
                       && hit.item.brush->patch )
                    m_hoverIsPatch = true;
            }
            if ( m_hover != was )
                g_nUpdateBits |= 1;
            UpdateHud();
        }

        // Plain click replaces the tool set; Shift+click toggles one member. The
        // first tool selects difference, but later picks preserve an explicit Q toggle.
        // Click never commits; RMB or Enter does.
        bool Click() override
        {
            if ( !m_hover )
            {
                if ( m_hoverIsPatch )           // name the specific refusal
                    Sys_Printf( "Boolean: that curve cannot cut — only curves straight "
                                "along one axis (cylinders, pipes, arches) can.  Use the "
                                "caulk brush inside it as the tool.\n" );
                else
                    Sys_Printf( "Boolean: that is not a usable solid — click another "
                                "one, or press Esc.\n" );
                return true;
            }

            const bool add = KiwiCmd_LastShift();
            if ( add )
            {
                for ( size_t i = 0; i < m_tools.size(); ++i )
                {
                    if ( m_tools[i] != m_hover )
                        continue;
                    m_tools.erase( m_tools.begin() + i );
                    if ( m_tools.empty() )
                        m_stage = KBOOL_PICK_TOOL;
                    UpdateHud();
                    Sys_Printf( "Boolean: solid removed — %i tool(s).\n",
                                (int)m_tools.size() );
                    g_nUpdateBits = -1;
                    return true;
                }
                m_tools.push_back( m_hover );
            }
            else
            {
                m_tools.clear();
                m_tools.push_back( m_hover );
            }

            if ( m_hover->patch )
                Sys_Printf( "Boolean: a curve tool cuts its outline through the brush, and "
                            "the curve stays with its ends trimmed flush to the brush.\n" );
            if ( m_stage == KBOOL_PICK_TOOL )
            {
                m_op    = KBOOL_DIFFERENCE;
                m_stage = KBOOL_LIVE;
                Sys_Printf( "Boolean: DIFFERENCE — Shift+click adds more tools, Q "
                            "switches to union, RMB / Enter applies, Esc drops the "
                            "last tool.\n" );
            }
            else
            {
                Sys_Printf( "Boolean: %i tool(s).\n", (int)m_tools.size() );
            }
            UpdateHud();
            g_nUpdateBits = -1;
            return true;
        }

        // The Shift-only marquee adds usable non-targets, suppresses duplicates,
        // and selects difference only when it first enters the live stage.
        void Marquee( int x0, int y0, int x1, int y1, bool crossing, bool shift ) override
        {
            (void)shift;                        // Shift-only rung: it is always true

            // Bound a whole-map marquee: every tool costs an N-plane carve per target.
            // KIWI (2026-09-28, user: "i cant select the middle blocks, I think i'm hitting a
            // maximum boolean #"): was 256 on the stack, and the cap is spent BEFORE the usable
            // filter below - a marquee over a building on a 2000-chunk terrain filled it with
            // patches it then threw away.  Heap buffer, 8192.
            const int   KBOOL_MARQUEE_MAX = 8192;
            std::vector<selbrush_t *> found( (size_t)KBOOL_MARQUEE_MAX, nullptr );
            const int n = KiwiBox_CollectBrushes( x0, y0, x1, y1, crossing,
                                                  &found[0], KBOOL_MARQUEE_MAX );

            int added = 0;
            for ( int i = 0; i < n; ++i )
            {
                selbrush_t *b = found[i];
                if ( !Sel_BrushLive( b ) || !Usable( b ) || IsTarget( b ) || IsTool( b ) )
                    continue;
                m_tools.push_back( b );
                ++added;
            }

            if ( !added )
            {
                Sys_Printf( "Boolean: nothing usable in that box — %i tool(s).\n",
                            (int)m_tools.size() );
                return;
            }

            if ( m_stage == KBOOL_PICK_TOOL )
            {
                m_op    = KBOOL_DIFFERENCE;
                m_stage = KBOOL_LIVE;
            }
            UpdateHud();
            Sys_Printf( "Boolean: %i solid(s) added — %i tool(s).\n",
                        added, (int)m_tools.size() );
            g_nUpdateBits = -1;
        }

        // Q toggles live operation; Esc pops tools before cancellation; Enter needs a tool.
        bool KeyDown( int vk, unsigned mods ) override
        {
            (void)mods;
            if ( vk == 0x51 && m_stage == KBOOL_LIVE )       // 'Q'
            {
                m_op = ( m_op == KBOOL_DIFFERENCE ) ? KBOOL_UNION : KBOOL_DIFFERENCE;
                UpdateHud();
                Sys_Printf( "Boolean: %s.\n", ( m_op == KBOOL_UNION )
                            ? "UNION — the solids become one (must stay convex)"
                            : "DIFFERENCE — the tools carve the selection" );
                g_nUpdateBits = -1;
                return true;
            }
            if ( vk == 0x1B && m_stage == KBOOL_LIVE )       // VK_ESCAPE
            {
                // Pop the most recent tool; after the last, one more Esc cancels.
                if ( m_tools.size() > 1 )
                {
                    m_tools.pop_back();
                    UpdateHud();
                    Sys_Printf( "Boolean: last solid dropped — %i tool(s).\n",
                                (int)m_tools.size() );
                    g_nUpdateBits = -1;
                    return true;
                }
                m_stage = KBOOL_PICK_TOOL;
                m_tools.clear();
                m_hover = 0;
                // Defensive no-op while WantsClicks prevents this command from parking.
                KiwiCmd_Resume();
                UpdateHud();
                Sys_Printf( "Boolean: tool dropped — click another solid (Esc again "
                            "cancels).\n" );
                g_nUpdateBits = -1;
                return true;
            }
            if ( vk == 0x0D && m_stage == KBOOL_PICK_TOOL )  // VK_RETURN
            {
                Sys_Printf( "Boolean: click the other solid first.\n" );
                return true;
            }
            return false;
        }

        // Difference fills tools red and only outlines already selected targets;
        // union fills both roles green. Fill and line budgets are whole-frame.
        // Spend newest-first, reserve the final hover outline, and report degradation
        // once so the most recent operand and next click target remain visible.
        void DrawWorld() override
        {
            const bool hoverOn = ( m_hover && Sel_BrushLive( m_hover )
                                && !IsTool( m_hover ) && m_hover->def );
            const int  reserve = hoverOn ? OutlineCost( m_hover->def ) : 0;

            int fillDropped = 0;
            int lineDropped = 0;

            if ( m_stage == KBOOL_LIVE && !m_tools.empty() )
            {
                int         filled = 0;
                const bool  uni  = ( m_op == KBOOL_UNION );
                const float *fill = uni ? KBOOL_UNION_RGBA : KBOOL_DIFF_RGBA;
                const float *line = uni ? KBOOL_UNION_LINE : KBOOL_DIFF_LINE;

                // Reverse walk spends preview budget on the newest operands first.
                for ( size_t i = m_tools.size(); i-- > 0; )
                {
                    if ( !Sel_BrushLive( m_tools[i] ) )
                        continue;
                    if ( filled >= KBOOL_MAX_FILL ) { ++fillDropped; continue; }
                    FillBrush( m_tools[i]->def, fill );
                    ++filled;
                }
                if ( uni )
                {
                    for ( size_t i = m_targets.size(); i-- > 0; )
                    {
                        if ( !Sel_BrushLive( m_targets[i] ) )
                            continue;
                        if ( filled >= KBOOL_MAX_FILL ) { ++fillDropped; continue; }
                        FillBrush( m_targets[i]->def, fill );
                        ++filled;
                    }
                }

                KiwiLines_Color( line[0], line[1], line[2] );
                for ( size_t i = m_tools.size(); i-- > 0; )
                {
                    if ( !Sel_BrushLive( m_tools[i] ) || !m_tools[i]->def )
                        continue;
                    // Stop before the reserved hover budget so drops remain countable.
                    if ( KiwiLines_Remaining() - reserve < OutlineCost( m_tools[i]->def ) )
                        { ++lineDropped; continue; }
                    OutlineBrush( m_tools[i]->def );
                }
                for ( size_t i = m_targets.size(); i-- > 0; )
                {
                    if ( !Sel_BrushLive( m_targets[i] ) || !m_targets[i]->def )
                        continue;
                    if ( KiwiLines_Remaining() - reserve < OutlineCost( m_targets[i]->def ) )
                        { ++lineDropped; continue; }
                    OutlineBrush( m_targets[i]->def );
                }
            }

            if ( hoverOn )
            {
                KiwiLines_Color( KBOOL_HOT_LINE[0], KBOOL_HOT_LINE[1], KBOOL_HOT_LINE[2] );
                OutlineBrush( m_hover->def );
            }

            NoteDegraded( fillDropped, lineDropped );
        }

        // Request the measured current outline cost; the framework clamps it to
        // KCMD_LINE_BUDGET_MAX while retaining the normal minimum budget.
        int LineBudget() const override
        {
            int n = KBOOL_LINE_HEADROOM;
            if ( m_stage == KBOOL_LIVE )
            {
                for ( size_t i = 0; i < m_tools.size(); ++i )
                    if ( Sel_BrushLive( m_tools[i] ) )
                        n += OutlineCost( m_tools[i]->def );
                for ( size_t i = 0; i < m_targets.size(); ++i )
                    if ( Sel_BrushLive( m_targets[i] ) )
                        n += OutlineCost( m_targets[i]->def );
            }
            if ( m_hover && Sel_BrushLive( m_hover ) )
                n += OutlineCost( m_hover->def );
            return n;
        }

        void Commit() override
        {
            if ( m_stage != KBOOL_LIVE || m_tools.empty() )
            {
                Sys_Printf( "Boolean: no tool solid was picked — nothing was done.\n" );
                Reset();
                return;
            }
            if ( m_op == KBOOL_UNION )
                DoUnion();
            else
                DoDifference();
            Reset();
            g_nUpdateBits = -1;
        }

        void Cancel() override
        {
            // Preview is draw-only; no geometry or undo bracket exists before Commit.
            Reset();
            g_nUpdateBits |= 1;
        }

    private:
        void Reset()
        {
            m_targets.clear();
            m_tools.clear();
            m_stage  = KBOOL_PICK_TOOL;
            m_op     = KBOOL_DIFFERENCE;
            m_hover  = 0;
            m_hud[0] = '\0';
            m_notedDegrade = false;     // one report per gesture
        }

        bool IsTarget( const selbrush_t *b ) const
        {
            for ( size_t i = 0; i < m_targets.size(); ++i )
                if ( m_targets[i] == b )
                    return true;
            return false;
        }

        // Tool sets are small enough that the predictable linear scan is appropriate.
        bool IsTool( const selbrush_t *b ) const
        {
            for ( size_t i = 0; i < m_tools.size(); ++i )
                if ( m_tools[i] == b )
                    return true;
            return false;
        }

        // Report preview truncation once per gesture; this runs every frame.
        void NoteDegraded( int fillDropped, int lineDropped )
        {
            if ( ( !fillDropped && !lineDropped ) || m_notedDegrade )
                return;
            m_notedDegrade = true;
            Sys_Printf( "Boolean: %i operand(s) are outlined but not tinted and %i "
                        "are not drawn at all — the preview budget is full.  The "
                        "NEWEST operands are the ones you can see; every operand in "
                        "the set is still applied on commit.\n",
                        fillDropped, lineDropped );
        }

        // Build every target carve before landing any replacement.
        void DoDifference()
        {
            struct carve_t
            {
                selbrush_t            *node;
                std::vector<brush_t *> pieces;
            };
            std::vector<carve_t> carves;
            int missed  = 0;
            int refused = 0;

            // Resolve live tool defs once so the synchronous carve uses a fixed set;
            // tools that disappeared during the gesture are ignored.
            std::vector<selbrush_t *> tools;
            std::vector<brush_t *>    toolDefs;
            // KIWI (2026-10-31): curve tools cut with an unlinked stand-in prism (curveTool_t)
            // and are kept, trimmed; the stand-ins are freed however this returns.
            std::vector<curveTool_t> curves;
            struct FreePrisms
            {
                std::vector<curveTool_t> *c;
                ~FreePrisms()
                {
                    for ( curveTool_t &ct : *c )
                        if ( ct.prism )
                            Brush_Free_R( ct.prism );
                }
            } freePrisms{ &curves };
            for ( size_t i = 0; i < m_tools.size(); ++i )
                if ( Sel_BrushLive( m_tools[i] ) && m_tools[i]->def )
                {
                    if ( m_tools[i]->patch )
                    {
                        curveTool_t ct;
                        const char *why = "unknown";
                        if ( !CurveToolPrism( m_tools[i], &ct, &why ) )
                        {
                            if ( ct.prism )
                                Brush_Free_R( ct.prism );
                            Sys_Printf( "Boolean: a curve tool was skipped — %s.\n", why );
                            continue;
                        }
                        curves.push_back( ct );
                        toolDefs.push_back( ct.prism );
                        continue;
                    }
                    tools.push_back( m_tools[i] );
                    toolDefs.push_back( m_tools[i]->def );
                }
            // KIWI (2026-11-01): a flat patch lying across a curve tool's end, inside its
            // outline - the end cap of an extruded arch solid - goes with the solid it closes,
            // as its brushes do; left standing it would hang in the opening.
            for ( int list = 0; list < 2 && !curves.empty(); ++list )
            {
                selbrush_t *head = list ? &selected_brushes : &active_brushes;
                for ( selbrush_t *b = head->next; b && b != head; b = b->next )
                {
                    const patchMesh_t *sp = ( b->def && b->patch ) ? b->def->patch : nullptr;
                    if ( !sp || IsTool( b ) || IsTarget( b ) )
                        continue;
                    bool endCap = false;
                    for ( size_t c = 0; c < curves.size() && !endCap; ++c )
                    {
                        if ( curves[c].node == b )
                            continue;
                        for ( int end = 0; end < 2 && !endCap; ++end )
                        {
                            const float at = end ? curves[c].hi : curves[c].lo;
                            endCap = true;
                            for ( int col = 0; col < sp->width && endCap; ++col )
                                for ( int row = 0; row < sp->height && endCap; ++row )
                                {
                                    const float *x = sp->ctrl[col][row].xyz;
                                    float r[3], q[2];
                                    Sub3( x, curves[c].pl.origin, r );
                                    KiwiCon_WorldToPlane( curves[c].pl, x, q );
                                    endCap = fabsf( Dot3( r, curves[c].axis ) - at ) < 0.1f
                                          && q[0] > curves[c].mn2[0] - 0.1f && q[0] < curves[c].mx2[0] + 0.1f
                                          && q[1] > curves[c].mn2[1] - 0.1f && q[1] < curves[c].mx2[1] + 0.1f;
                                }
                        }
                    }
                    if ( endCap )
                        tools.push_back( b );
                }
            }
            if ( toolDefs.empty() )
            {
                Sys_Printf( "Boolean: no tool solid is still there — nothing was "
                            "done.\n" );
                return;
            }

            // Scope all diagnostics to this commit.
            KiwiBool_ResetCarveReport();
            KiwiCsg_FailHintClear();          // KIWI: refusals below draw the new hint

            for ( size_t i = 0; i < m_targets.size(); ++i )
            {
                selbrush_t *node = m_targets[i];
                if ( !Sel_BrushLive( node ) || IsTool( node ) )
                    continue;

                // Build in place so the piece vector has one owner; refusals leave it empty.
                carves.push_back( carve_t() );
                carves.back().node = node;

                const char *why = "unknown";
                const carveResult_t r = CarveTarget( node->def, &toolDefs[0],
                                                     (int)toolDefs.size(),
                                                     &carves.back().pieces, &why );
                if ( r == KBOOL_CARVED )
                    continue;

                carves.pop_back();
                if ( r == KBOOL_MISS )
                {
                    ++missed;
                    // Name non-overlap versus a near-tangent validity-gate miss.
                    Sys_Printf( "Boolean: one brush left untouched — %s.\n",
                                ( why && *why ) ? why : "the tool(s) did not reach it" );
                }
                else
                {
                    ++refused;
                    Sys_Printf( "Boolean: one brush left untouched — %s.\n",
                                why ? why : "invalid geometry" );
                    // KIWI: failure hint - where the tools meet it, else the whole brush.
                    bool hinted = false;
                    for ( size_t t = 0; t < toolDefs.size(); ++t )
                        hinted |= KiwiCsg_FailHintAddOverlap( node->def, toolDefs[t] );
                    if ( !hinted )
                        KiwiCsg_FailHintAddBrush( node->def );
                }
            }

            if ( carves.empty() )
            {
                // Do not create an empty undo record when nothing changed.
                Sys_Printf( "Boolean: nothing was carved (%i brush(es) did not meet "
                            "the tool(s), %i refused).\n", missed, refused );
                return;
            }

            // Report validity-policy diagnostics for a commit that produced carves.
            if ( s_carveSliverDrops > 0 )
                Sys_Printf( "Boolean: %i fragment(s) came out below the validity gate "
                            "(zero-area / collapsed) and were dropped rather than "
                            "refusing the whole carve.\n", s_carveSliverDrops );
            if ( s_carveRefusals > 0 )
                Sys_Printf( "Boolean: %i individual cut(s) were refused and skipped — "
                            "the hole may be incomplete where those planes fell.\n",
                            s_carveRefusals );
            if ( s_carveCarried > 0 )
                Sys_Printf( "Boolean: %i running intersection(s) were outside the "
                            "validity gate and were carried on anyway (they are "
                            "intermediates and are never landed).\n", s_carveCarried );

            // KIWI: each curve's ends go onto the faces of the brushes it cut, planned while
            // those brushes still stand.
            int trimmedCurves = 0, curveCaps = 0;
            std::vector<const brush_t *> cut;
            std::vector<entity_s *>      cutOwners;
            for ( const carve_t &cv : carves )
            {
                cut.push_back( cv.node->def );
                cutOwners.push_back( cv.node->owner );
            }
            for ( curveTool_t &ct : curves )
                if ( !PlanCurveTrim( &ct, cut ) )
                {
                    ct.trimmed.clear();
                    ct.caps.clear();             // it passes beside the brushes: no wall to cap
                }

            // Clear typed selection before freeing nodes; keep the legacy list for undo cloning.
            Sel_Clear( KiwiSel() );

            KiwiCmd_UndoBegin( "boolean difference" );

            // KIWI: the curves are reshaped in place, so the record covers them first.
            for ( curveTool_t &ct : curves )
            {
                if ( !Sel_BrushLive( ct.node ) )
                    continue;
                // KIWI: the caps go in while the cut brushes - their faces' material - still stand.
                if ( !ct.caps.empty() )
                    curveCaps += LandCurveCaps( ct, cut, cutOwners );
                if ( ct.trimmed.empty() || !ct.changed )
                    continue;
                Undo_AddBrush( (entity_brush_s *)ct.node->def );
                patchMesh_t *p = ct.node->def->patch;
                const int na = CurveAround( p, ct.alongRows ), nl = CurveAlong( p, ct.alongRows );
                for ( int i = 0; i < na; ++i )
                    for ( int j = 0; j < nl; ++j )
                        Copy3( &ct.trimmed[( (size_t)i * nl + j ) * 3], CurvePt( p, ct.alongRows, i, j ) );
                // Patch_UpdateSelected_0's own refresh (pmesh.cpp 0x43D800); bDirty stays clear.
                Patch_Rebuild( p, 1 );
                ++trimmedCurves;
            }

            // KIWI: a kept curve faces into the opening it cut (CurveFacesWall); reversing one
            // parametric direction turns it round (KiwiPrim_BuildPatchBackface).
            int flippedCurves = 0;
            for ( curveTool_t &ct : curves )
            {
                if ( !Sel_BrushLive( ct.node ) || ct.trimmed.empty() || !CurveFacesWall( ct ) )
                    continue;                    // (empty trimmed: it passed beside the brushes)
                Undo_AddBrush( (entity_brush_s *)ct.node->def );   // self-deduplicating
                patchMesh_t *p = ct.node->def->patch;
                for ( int col = 0; col < p->width; ++col )
                    for ( int row = 0; row < p->height / 2; ++row )
                    {
                        const drawVert_t v = p->ctrl[col][row];
                        p->ctrl[col][row] = p->ctrl[col][p->height - 1 - row];
                        p->ctrl[col][p->height - 1 - row] = v;
                    }
                Patch_Rebuild( p, 1 );
                ++flippedCurves;
            }

            // Picked tools are consumed but excluded from selected_brushes, so the
            // bracket head did not clone them. Cover each explicitly, entity first;
            // the target guard avoids duplicate restoration if invariants are breached.
            for ( size_t t = 0; t < tools.size(); ++t )
            {
                if ( IsTarget( tools[t] ) )
                    continue;
                entity_s *owner = tools[t]->def->owner;
                if ( owner && owner->eclass && owner->eclass->fixedsize )
                    Undo_AddEntity( (entity_s *)owner );
                Undo_AddBrush( (entity_brush_s *)tools[t]->def );
            }

            int landed = 0;
            std::vector<brush_t *> pieces;
            for ( size_t c = 0; c < carves.size(); ++c )
            {
                carve_t &cv = carves[c];
                // Land replacements before freeing their source so its owner never goes empty.
                for ( size_t p = 0; p < cv.pieces.size(); ++p )
                {
                    selbrush_t *inst = Brush_AddToList( cv.pieces[p], cv.node->owner );
                    if ( inst->next || inst->prev )
                        Com_Error( ERR_FATAL, "Brush_AddToList: already linked" );
                    Brush_AddToList2( inst );
                    pieces.push_back( cv.pieces[p] );
                    ++landed;
                }
                Brush_Free( cv.node );
            }

            // KIWI (2026-11-01, user: "the top ceiling of the arch should "match brush" with the
            // lower walls by default"): each kept curve continues the texture of the carved face
            // it springs from - a doorway's side wall - wrapped over (KiwiPatch_TextureFromFace).
            // A doorway's sides may be brushes the cut never touched (a wall built round its
            // opening): after the new pieces, every brush in the map.
            std::vector<brush_t *> mapBrushes;
            for ( int list = 0; list < 2 && !curves.empty(); ++list )
            {
                selbrush_t *head = list ? &selected_brushes : &active_brushes;
                for ( selbrush_t *b = head->next; b && b != head; b = b->next )
                    if ( b->def && !b->def->patch && !IsTool( b ) )
                        mapBrushes.push_back( b->def );
            }
            int matchedCurves = 0;
            for ( curveTool_t &ct : curves )
            {
                if ( !Sel_BrushLive( ct.node ) )
                    continue;
                patchMesh_t  *p   = ct.node->def->patch;
                const face_t *src = KiwiPatch_FindSourceFace( p, pieces.data(), (int)pieces.size() );
                if ( !src )
                    src = KiwiPatch_FindSourceFace( p, mapBrushes.data(), (int)mapBrushes.size() );
                if ( !src )
                    continue;
                Undo_AddBrush( (entity_brush_s *)ct.node->def );   // self-deduplicating
                // User: "match the ceiling to both sides of the arch": folded at the crown, and
                // the wall it comes down onto scrolls to meet it.  New pieces are undone by this
                // record already; an untouched brush is covered first.
                kpatchFaceRef_t farFaces[64];
                float scroll[2];
                int nFar = KiwiPatch_MatchTexture( p, src, pieces.data(), (int)pieces.size(), farFaces, 64, scroll );
                if ( nFar < 0 )
                    continue;
                ++matchedCurves;
                if ( !nFar )
                    nFar = KiwiPatch_MatchTexture( p, src, mapBrushes.data(), (int)mapBrushes.size(), farFaces, 64, scroll );
                for ( int i = 0; i < nFar; ++i )
                {
                    brush_t *fb = farFaces[i].brush;
                    if ( std::find( pieces.begin(), pieces.end(), fb ) == pieces.end() )
                    {
                        entity_s *owner = fb->owner;
                        if ( owner && owner->eclass && owner->eclass->fixedsize )
                            Undo_AddEntity( owner );
                        Undo_AddBrush( (entity_brush_s *)fb );
                    }
                    KiwiPatch_ScrollFace( fb, farFaces[i].face, scroll );
                }
            }

            // Free tools only after every replacement has landed; tools may share an
            // owner with targets. Brush_Free removes only the instance, unlike
            // Select_Delete's additional empty-owner behavior.
            for ( size_t t = 0; t < tools.size(); ++t )
                Brush_Free( tools[t] );
            m_tools.clear();

            Sys_Printf( "Boolean: difference — %i brush(es) carved into %i, and %i "
                        "tool(s) were consumed (%i missed, %i refused).\n",
                        (int)carves.size(), landed, (int)tools.size(),
                        missed, refused );
            if ( !curves.empty() )
                Sys_Printf( "Boolean: %i curve tool(s) kept, %i with their ends trimmed flush "
                            "to the brush, %i cap patch(es) round them, %i textured on from "
                            "the wall, %i turned to face the opening.\n", (int)curves.size(),
                            trimmedCurves, curveCaps, matchedCurves, flippedCurves );
        }

        // Select live targets and tools, then delegate to CSG_Merge (csg.cpp:572).
        // That handler owns geometry, undo, reporting, entity checks, and the rule
        // that a classic brush union must have a convex hull.
        void DoUnion()
        {
            int live = 0;
            for ( size_t i = 0; i < m_targets.size(); ++i )
                if ( Sel_BrushLive( m_targets[i] ) && !IsTool( m_targets[i] ) )
                    ++live;
            if ( live < 1 )
            {
                Sys_Printf( "Boolean: nothing left to union with.\n" );
                return;
            }

            int tools = 0;
            Sel_Clear( KiwiSel() );
            Select_Deselect( 1 );
            for ( size_t i = 0; i < m_targets.size(); ++i )
                if ( Sel_BrushLive( m_targets[i] ) && !IsTool( m_targets[i] ) )
                    Select_Brush( m_targets[i], 0, 0, 0 );
            for ( size_t i = 0; i < m_tools.size(); ++i )
                if ( Sel_BrushLive( m_tools[i] ) )
                {
                    Select_Brush( m_tools[i], 0, 0, 0 );
                    ++tools;
                }

            Sys_Printf( "Boolean: union of %i brush(es) + %i tool(s)...\n",
                        live, tools );
            Radiant_ExecCommand( (unsigned int)KBOOL_ID_MERGE );
        }

        void UpdateHud()
        {
            if ( m_stage == KBOOL_PICK_TOOL )
            {
                _snprintf( m_hud, sizeof( m_hud ),
                           "boolean  %i solid(s) selected  %s", (int)m_targets.size(),
                           m_hover ? "click this solid to use as the tool"
                                   : "click the other solid" );
            }
            else
            {
                // Counts make successful additive picks visible even without motion.
                _snprintf( m_hud, sizeof( m_hud ),
                           "boolean  %s  %i target(s) + %i tool(s)  (Q switches)",
                           ( m_op == KBOOL_UNION ) ? "UNION" : "DIFFERENCE",
                           (int)m_targets.size(), (int)m_tools.size() );
            }
            m_hud[sizeof( m_hud ) - 1] = '\0';
        }

        enum stage_t { KBOOL_PICK_TOOL = 0, KBOOL_LIVE };
        enum op_t    { KBOOL_DIFFERENCE = 0, KBOOL_UNION };

    public:
        // KIWI (2026-10-31): test-only, the Click() a scripted probe cannot aim.
        void TestPickTool( selbrush_t *b )
        {
            m_tools.clear();
            m_tools.push_back( b );
            m_op    = KBOOL_DIFFERENCE;
            m_stage = KBOOL_LIVE;
        }
        // ...and a Shift-click's further tool.  A target picked as a tool stops being a target.
        void TestAddTool( selbrush_t *b )
        {
            m_targets.erase( std::remove( m_targets.begin(), m_targets.end(), b ), m_targets.end() );
            m_tools.push_back( b );
        }

    private:
        std::vector<selbrush_t *> m_targets;
        // Pick order controls Esc and fragment topology, though not the difference volume.
        std::vector<selbrush_t *> m_tools;
        selbrush_t *m_hover = 0;
        // Latch a patch-specific refusal for Click.
        bool        m_hoverIsPatch = false;
        stage_t     m_stage = KBOOL_PICK_TOOL;
        op_t        m_op    = KBOOL_DIFFERENCE;
        // One preview-degradation report per gesture.
        bool        m_notedDegrade = false;
        char        m_hud[160] = { 0 };
    };

    KiwiBooleanCommand s_boolean;
}

// Palette predicate.
bool KiwiBool_CanBoolean()
{
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
        if ( Usable( b ) )
            return true;
    return false;
}

// Registration and lookup.
void KiwiBool_RegisterCommands()
{
    Radiant_RegisterCommand( "KiwiBoolean", 0, 0, KIWI_CMD_BOOLEAN );
}

KiwiEditorCommand *KiwiBool_CommandForId( int commandId )
{
    if ( commandId == KIWI_CMD_BOOLEAN )
        return &s_boolean;
    return 0;
}

bool KiwiBool_TestPickTool( selbrush_t *tool )
{
    if ( KiwiCmd_Active() != &s_boolean || !tool )
        return false;
    s_boolean.TestPickTool( tool );
    return true;
}

bool KiwiBool_TestAddTool( selbrush_t *tool )
{
    if ( KiwiCmd_Active() != &s_boolean || !tool )
        return false;
    s_boolean.TestAddTool( tool );
    return true;
}

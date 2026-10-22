#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// See kiwi_pasteplace.h.

#include "stdafx.h"
#include "qe3.h"
#include "kiwi_pasteplace.h"
#include "kiwi_command.h"
#include "kiwi_droptrace.h"            // KiwiDrop_IsModelEntity / KiwiDrop_GetModelInfo
#include "kiwi_outliner.h"             // group paths (brush-group replace)
#include "kiwi_ported.h"
#include "kiwi_selection.h"            // Sel_InvalidateFromLegacy
#include "kiwi_vec.h"
#include "prefs.h"                     // texture / rotate / lightmap lock

#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

// Ported entry points (verified against their definitions).
extern bool        g_kiwiImportInCallerUndo;                                  // map.cpp (KIWI hook)
extern bool        RadiantClipboard_HasMapText();                             // entity.cpp (KIWI tail)
extern void        RadiantClipboard_Paste();                                  // entity.cpp

namespace
{
    struct ppPose_t
    {
        float       origin[3];
        float       angles[3];
        float       scale;
        std::string modelName;          // the "model" key, lower case, forward slashes
        float       size;               // bounds volume - the fallback "primary model" measure
        std::string angleText;          // the target's own "angles" string, reused verbatim
    };

    const char *KeyOf( const entity_s_def *def, const char *key )
    {
        if ( def )
            for ( const epair_t *ep = def->epairs; ep; ep = ep->next )
                if ( ep->key && !_stricmp( ep->key, key ) )
                    return ep->value ? ep->value : "";
        return "";
    }

    // (forward, left, up) rows from pitch / yaw / roll - AnglesToAxis.
    void AnglesAxis( const float *ang, float out[3][3] )
    {
        const float k = (float)KIWI_PI / 180.0f;
        const float sp = sinf( ang[0] * k ), cp = cosf( ang[0] * k );
        const float sy = sinf( ang[1] * k ), cy = cosf( ang[1] * k );
        const float sr = sinf( ang[2] * k ), cr = cosf( ang[2] * k );
        out[0][0] = cp * cy;                out[0][1] = cp * sy;                out[0][2] = -sp;
        out[1][0] = sr * sp * cy - cr * sy; out[1][1] = sr * sp * sy + cr * cy; out[1][2] = sr * cp;
        out[2][0] = cr * sp * cy + sr * sy; out[2][1] = cr * sp * sy - sr * cy; out[2][2] = cr * cp;
    }

    // KIWI (2026-09-18, user: "the wall vanishes and I see nothing ... do a comparison via
    // name, and if that doesn't work take the biggest one by size"): the first version read
    // the pose through KiwiDrop_GetModelInfo, which needs the XModel RESIDENT with valid
    // bounds.  A model pasted a microsecond ago is not, so every pasted model failed the read,
    // no anchor was found, the group stayed where it was copied (invisible, on top of its
    // originals) - and the target was already deleted.  The pose now comes straight from
    // the entity: origin, the `angles` / `angle` keys, `modelscale`, and the `model` NAME.
    bool PoseOf( selbrush_t *node, ppPose_t *out )
    {
        if ( !node || !node->owner || !KiwiDrop_IsModelEntity( node ) )
            return false;
        const entity_s_def *def = (const entity_s_def *)node->owner->def;
        if ( !def )
            return false;
        for ( int k = 0; k < 3; ++k )
            out->origin[k] = def->origin[k];
        out->angles[0] = out->angles[1] = out->angles[2] = 0.0f;
        out->angleText = KeyOf( def, "angles" );
        if ( out->angleText.empty()
          || sscanf( out->angleText.c_str(), "%f %f %f", &out->angles[0], &out->angles[1], &out->angles[2] ) != 3 )
        {
            out->angles[0] = out->angles[2] = 0.0f;
            out->angles[1] = (float)atof( KeyOf( def, "angle" ) );     // legacy yaw-only key
        }
        out->scale = 1.0f;
        {
            const float s = (float)atof( KeyOf( def, "modelscale" ) );
            if ( s > 0.0f )
                out->scale = s;
        }
        out->modelName = KeyOf( def, "model" );
        for ( size_t i = 0; i < out->modelName.size(); ++i )
        {
            char &c = out->modelName[i];
            if ( c == '\\' ) c = '/';
            else if ( c >= 'A' && c <= 'Z' ) c = (char)( c - 'A' + 'a' );
        }
        // size: the real model bounds when it is loaded, else the proxy brush
        float mins[3], maxs[3], ang[3], sc, org[3];
        if ( KiwiDrop_GetModelInfo( node, mins, maxs, ang, &sc, org ) )
            out->size = ( maxs[0] - mins[0] ) * ( maxs[1] - mins[1] ) * ( maxs[2] - mins[2] ) * sc * sc * sc;
        else if ( node->def )
            out->size = ( node->def->maxs[0] - node->def->mins[0] ) * ( node->def->maxs[1] - node->def->mins[1] )
                      * ( node->def->maxs[2] - node->def->mins[2] );
        else
            out->size = 0.0f;
        return true;
    }

    int SelectedModelCount()
    {
        int n = 0;
        for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
            if ( KiwiDrop_IsModelEntity( b ) )
                ++n;
        return n;
    }

    // ── brush groups (KIWI 2026-10-19, user: "paste in place should work with brush groups
    // too. Correlate the matching one of the group to determine the placement and where to
    // match") ─────────────────────────────────────────────────────────────────────────────
    // A selection holding brushes is replaced GROUP by GROUP.  The rigid transform comes from
    // a clipboard brush congruent to one of the target group's brushes (or a same-name model);
    // every candidate is scored by how many of the target's members it lands a clipboard
    // member on, so a symmetric box cannot pick the wrong turn when the rest of the group says
    // otherwise.

    bool IsPlainBrush( selbrush_t *b )
    {
        if ( !b || !b->def || b->patch || !b->owner || !b->owner->def )
            return false;
        const entity_s_def *def = (const entity_s_def *)b->owner->def;
        return !( def->eclass && *(const int *)&def->eclass->fixedsize ) && b->def->faceCount >= 4;
    }

    bool SelectionHasBrushes()
    {
        for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
            if ( b->patch || IsPlainBrush( b ) )
                return true;
        return false;
    }

    struct ppShape_t
    {
        std::vector<double> v;          // unique vertices, xyz triples
        double              c[3];       // their mean
        std::vector<double> radii;      // sorted distances from c
        int                 faces;
        double              volume;     // bounds volume, to try the big brushes first
        double              yaw;        // heading of its biggest upright side (radians)
        bool                hasYaw;     // false: no upright side
    };

    const double PP_TOL = 0.1;          // units; float planepts of a turned copy drift ~0.01

    bool ShapeOf( selbrush_t *b, ppShape_t *out )
    {
        if ( !IsPlainBrush( b ) )
            return false;
        out->v.clear();
        out->radii.clear();
        out->faces = b->def->faceCount;
        double mins[3] = { 1e30, 1e30, 1e30 }, maxs[3] = { -1e30, -1e30, -1e30 };
        for ( int f = 0; f < b->def->faceCount; ++f )
        {
            const winding_t *w = b->def->faces[f].w;
            for ( int i = 0; w && i < w->numpoints; ++i )
            {
                const double p[3] = { w->p[i][0], w->p[i][1], w->p[i][2] };
                bool dup = false;
                for ( size_t j = 0; j < out->v.size() && !dup; j += 3 )
                    dup = fabs( out->v[j] - p[0] ) < 0.01 && fabs( out->v[j + 1] - p[1] ) < 0.01
                       && fabs( out->v[j + 2] - p[2] ) < 0.01;
                if ( dup )
                    continue;
                for ( int k = 0; k < 3; ++k )
                {
                    out->v.push_back( p[k] );
                    if ( p[k] < mins[k] ) mins[k] = p[k];
                    if ( p[k] > maxs[k] ) maxs[k] = p[k];
                }
            }
        }
        const size_t n = out->v.size() / 3;
        if ( n < 4 )
            return false;
        out->c[0] = out->c[1] = out->c[2] = 0.0;
        for ( size_t j = 0; j < out->v.size(); j += 3 )
            for ( int k = 0; k < 3; ++k )
                out->c[k] += out->v[j + k] / (double)n;
        for ( size_t j = 0; j < out->v.size(); j += 3 )
        {
            const double d[3] = { out->v[j] - out->c[0], out->v[j + 1] - out->c[1], out->v[j + 2] - out->c[2] };
            out->radii.push_back( sqrt( d[0] * d[0] + d[1] * d[1] + d[2] * d[2] ) );
        }
        std::sort( out->radii.begin(), out->radii.end() );
        out->volume = ( maxs[0] - mins[0] ) * ( maxs[1] - mins[1] ) * ( maxs[2] - mins[2] );

        // the side direction the close-size fallback turns by
        out->yaw    = 0.0;
        out->hasYaw = false;
        double bestArea = 0.0;
        for ( int f = 0; f < b->def->faceCount; ++f )
        {
            const face_t    *face = &b->def->faces[f];
            const winding_t *w    = face->w;
            if ( !w || w->numpoints < 3 || fabs( face->plane.normal[2] ) > 0.1f )
                continue;
            double area = 0.0;
            for ( int i = 1; i + 1 < w->numpoints; ++i )
            {
                const double a[3] = { w->p[i][0] - w->p[0][0], w->p[i][1] - w->p[0][1], w->p[i][2] - w->p[0][2] };
                const double c[3] = { w->p[i + 1][0] - w->p[0][0], w->p[i + 1][1] - w->p[0][1], w->p[i + 1][2] - w->p[0][2] };
                const double x = a[1] * c[2] - a[2] * c[1], y = a[2] * c[0] - a[0] * c[2], z = a[0] * c[1] - a[1] * c[0];
                area += 0.5 * sqrt( x * x + y * y + z * z );
            }
            if ( area > bestArea + 1e-6 )
            {
                bestArea    = area;
                out->yaw    = atan2( (double)face->plane.normal[1], (double)face->plane.normal[0] );
                out->hasYaw = true;
            }
        }
        return true;
    }

    bool SameShape( const ppShape_t &a, const ppShape_t &b )
    {
        if ( a.faces != b.faces || a.radii.size() != b.radii.size() )
            return false;
        for ( size_t i = 0; i < a.radii.size(); ++i )
            if ( fabs( a.radii[i] - b.radii[i] ) > PP_TOL )
                return false;
        return true;
    }

    // column convention: world p' = R p + t
    struct ppXform_t
    {
        double R[3][3];
        double t[3];
    };

    void XformPoint( const ppXform_t &X, const double *p, double *out )
    {
        for ( int r = 0; r < 3; ++r )
            out[r] = X.R[r][0] * p[0] + X.R[r][1] * p[1] + X.R[r][2] * p[2] + X.t[r];
    }

    bool Near( const double *a, const double *b, double tol )
    {
        return fabs( a[0] - b[0] ) < tol && fabs( a[1] - b[1] ) < tol && fabs( a[2] - b[2] ) < tol;
    }

    // Every vertex of p lands on a vertex of q.
    bool MapsOnto( const ppXform_t &X, const ppShape_t &p, const ppShape_t &q )
    {
        for ( size_t i = 0; i < p.v.size(); i += 3 )
        {
            double m[3];
            XformPoint( X, &p.v[i], m );
            bool hit = false;
            for ( size_t j = 0; j < q.v.size() && !hit; j += 3 )
                hit = Near( m, &q.v[j], PP_TOL );
            if ( !hit )
                return false;
        }
        return true;
    }

    // Orthonormal frame (columns) from two non-parallel vectors.
    bool Frame( const double *a, const double *b, double F[3][3] )
    {
        double la = sqrt( a[0] * a[0] + a[1] * a[1] + a[2] * a[2] );
        if ( la < 1e-6 )
            return false;
        const double e1[3] = { a[0] / la, a[1] / la, a[2] / la };
        const double d = b[0] * e1[0] + b[1] * e1[1] + b[2] * e1[2];
        double e2[3] = { b[0] - d * e1[0], b[1] - d * e1[1], b[2] - d * e1[2] };
        const double l2 = sqrt( e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2] );
        if ( l2 < 1e-3 )
            return false;
        for ( int k = 0; k < 3; ++k )
            e2[k] /= l2;
        const double e3[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
        for ( int r = 0; r < 3; ++r )
        {
            F[r][0] = e1[r];
            F[r][1] = e2[r];
            F[r][2] = e3[r];
        }
        return true;
    }

    // Every proper rotation (plus translation) that lays p exactly onto q.
    void ShapeCandidates( const ppShape_t &p, const ppShape_t &q, std::vector<ppXform_t> &out )
    {
        const size_t n = p.v.size() / 3;
        // a = farthest vertex from the centre, b = the one most off the centre-a line
        size_t ia = 0, ib = 0;
        double best = -1.0;
        double rel[64][3];
        if ( n > 64 )
            return;
        for ( size_t i = 0; i < n; ++i )
        {
            for ( int k = 0; k < 3; ++k )
                rel[i][k] = p.v[i * 3 + k] - p.c[k];
            const double r = rel[i][0] * rel[i][0] + rel[i][1] * rel[i][1] + rel[i][2] * rel[i][2];
            if ( r > best ) { best = r; ia = i; }
        }
        best = -1.0;
        for ( size_t i = 0; i < n; ++i )
        {
            const double *a = rel[ia], *v = rel[i];
            const double cx = a[1] * v[2] - a[2] * v[1], cy = a[2] * v[0] - a[0] * v[2], cz = a[0] * v[1] - a[1] * v[0];
            const double s = cx * cx + cy * cy + cz * cz;
            if ( s > best ) { best = s; ib = i; }
        }
        double FP[3][3];
        if ( !Frame( rel[ia], rel[ib], FP ) )
            return;
        auto len = []( const double *v ) { return sqrt( v[0] * v[0] + v[1] * v[1] + v[2] * v[2] ); };
        const double ra = len( rel[ia] ), rb = len( rel[ib] );
        const double dab[3] = { rel[ia][0] - rel[ib][0], rel[ia][1] - rel[ib][1], rel[ia][2] - rel[ib][2] };
        const double lab = len( dab );

        const size_t m = q.v.size() / 3;
        for ( size_t i = 0; i < m; ++i )
        {
            const double qa[3] = { q.v[i * 3] - q.c[0], q.v[i * 3 + 1] - q.c[1], q.v[i * 3 + 2] - q.c[2] };
            if ( fabs( len( qa ) - ra ) > PP_TOL )
                continue;
            for ( size_t j = 0; j < m; ++j )
            {
                if ( j == i )
                    continue;
                const double qb[3] = { q.v[j * 3] - q.c[0], q.v[j * 3 + 1] - q.c[1], q.v[j * 3 + 2] - q.c[2] };
                const double qd[3] = { qa[0] - qb[0], qa[1] - qb[1], qa[2] - qb[2] };
                if ( fabs( len( qb ) - rb ) > PP_TOL || fabs( len( qd ) - lab ) > PP_TOL )
                    continue;
                double FQ[3][3];
                if ( !Frame( qa, qb, FQ ) )
                    continue;
                ppXform_t X;
                for ( int r = 0; r < 3; ++r )
                    for ( int c = 0; c < 3; ++c )
                        X.R[r][c] = FQ[r][0] * FP[c][0] + FQ[r][1] * FP[c][1] + FQ[r][2] * FP[c][2];
                for ( int r = 0; r < 3; ++r )
                    X.t[r] = q.c[r] - ( X.R[r][0] * p.c[0] + X.R[r][1] * p.c[1] + X.R[r][2] * p.c[2] );
                if ( MapsOnto( X, p, q ) )
                    out.push_back( X );
                if ( out.size() > 4096 )
                    return;
            }
        }
    }

    // The pose pair as a transform: R carries the clipboard model's axes onto the target's.
    void PoseXform( const ppPose_t &A, const ppPose_t &T, ppXform_t *X )
    {
        float ax[3][3], tx[3][3];
        AnglesAxis( A.angles, ax );
        AnglesAxis( T.angles, tx );
        for ( int r = 0; r < 3; ++r )
            for ( int c = 0; c < 3; ++c )
                X->R[r][c] = (double)tx[0][r] * ax[0][c] + (double)tx[1][r] * ax[1][c] + (double)tx[2][r] * ax[2][c];
        for ( int r = 0; r < 3; ++r )
            X->t[r] = T.origin[r] - ( X->R[r][0] * A.origin[0] + X->R[r][1] * A.origin[1] + X->R[r][2] * A.origin[2] );
    }

    struct ppMembers_t
    {
        std::vector<ppShape_t> shapes;
        std::vector<ppPose_t>  poses;
    };

    void MembersOf( const std::vector<selbrush_t *> &nodes, ppMembers_t &out )
    {
        for ( size_t i = 0; i < nodes.size(); ++i )
        {
            ppShape_t s;
            ppPose_t  p;
            if ( ShapeOf( nodes[i], &s ) )
                out.shapes.push_back( s );
            else if ( PoseOf( nodes[i], &p ) )
                out.poses.push_back( p );
        }
    }

    // Target members a clipboard member lands on under X.
    int Score( const ppXform_t &X, const ppMembers_t &clip, const ppMembers_t &target )
    {
        int score = 0;
        for ( size_t t = 0; t < target.shapes.size(); ++t )
        {
            const ppShape_t &q = target.shapes[t];
            for ( size_t c = 0; c < clip.shapes.size(); ++c )
            {
                const ppShape_t &p = clip.shapes[c];
                double pc[3];
                XformPoint( X, p.c, pc );
                if ( Near( pc, q.c, PP_TOL ) && SameShape( p, q ) && MapsOnto( X, p, q ) )
                {
                    ++score;
                    break;
                }
            }
        }
        for ( size_t t = 0; t < target.poses.size(); ++t )
        {
            const ppPose_t &T = target.poses[t];
            const double to[3] = { T.origin[0], T.origin[1], T.origin[2] };
            for ( size_t c = 0; c < clip.poses.size(); ++c )
            {
                const ppPose_t &A = clip.poses[c];
                if ( A.modelName != T.modelName )
                    continue;
                const double ao[3] = { A.origin[0], A.origin[1], A.origin[2] };
                double m[3];
                XformPoint( X, ao, m );
                if ( Near( m, to, 0.5 ) )
                {
                    ++score;
                    break;
                }
            }
        }
        return score;
    }

    // Bounds of p's vertices under X, measured in the frame turned by `yaw` about Z.
    void FrameBounds( const ppShape_t &p, const ppXform_t *X, double yaw, double mins[3], double maxs[3] )
    {
        const double cy = cos( yaw ), sy = sin( yaw );
        for ( int k = 0; k < 3; ++k ) { mins[k] = 1e30; maxs[k] = -1e30; }
        for ( size_t i = 0; i < p.v.size(); i += 3 )
        {
            double w[3] = { p.v[i], p.v[i + 1], p.v[i + 2] };
            if ( X )
            {
                double m[3];
                XformPoint( *X, w, m );
                w[0] = m[0]; w[1] = m[1]; w[2] = m[2];
            }
            const double l[3] = { cy * w[0] + sy * w[1], -sy * w[0] + cy * w[1], w[2] };
            for ( int k = 0; k < 3; ++k )
            {
                if ( l[k] < mins[k] ) mins[k] = l[k];
                if ( l[k] > maxs[k] ) maxs[k] = l[k];
            }
        }
    }

    // KIWI (2026-10-19, user: "it doesn't work, im trying to get it so i can paste these
    // enhanced stair steps on top of the old steps"): a step built afresh is never an exact
    // copy of the old one, so no brush is congruent.  Close-size fallback: the clipboard brush
    // whose footprint best fits the target's biggest brush, turned about Z only (by the two
    // brushes' upright sides, in quarter turns; the smallest turn on a tie), footprint centred
    // on it.  Height: a brush as tall as the old one sits exactly on it; otherwise the whole
    // paste's TOP meets the old top - the tread stays where it was.  Footprints must agree
    // within 20% (2 u at least) on both sides.
    bool MatchCloseSize( const ppMembers_t &clip, const ppMembers_t &target, ppXform_t *best )
    {
        if ( clip.shapes.empty() || target.shapes.empty() )
            return false;
        size_t qi = 0;
        for ( size_t t = 1; t < target.shapes.size(); ++t )
            if ( target.shapes[t].volume > target.shapes[qi].volume )
                qi = t;
        const ppShape_t &q = target.shapes[qi];
        const double frame = q.hasYaw ? q.yaw : 0.0;
        double qmin[3], qmax[3];
        FrameBounds( q, nullptr, frame, qmin, qmax );
        const double qdx = qmax[0] - qmin[0], qdy = qmax[1] - qmin[1], qdz = qmax[2] - qmin[2];

        // the whole paste's top (a turn about Z leaves heights alone)
        double clipTop = -1e30;
        for ( size_t c = 0; c < clip.shapes.size(); ++c )
            for ( size_t i = 2; i < clip.shapes[c].v.size(); i += 3 )
                if ( clip.shapes[c].v[i] > clipTop )
                    clipTop = clip.shapes[c].v[i];

        bool   found = false;
        double bestErr = 1e30, bestTrace = -10.0;
        for ( size_t c = 0; c < clip.shapes.size(); ++c )
        {
            const ppShape_t &p = clip.shapes[c];
            const double base = ( q.hasYaw && p.hasYaw ) ? q.yaw - p.yaw : 0.0;
            for ( int k = 0; k < 4; ++k )
            {
                const double a = base + k * ( KIWI_PI * 0.5 );
                ppXform_t X;
                const double ca = cos( a ), sa = sin( a );
                const double R[3][3] = { { ca, -sa, 0 }, { sa, ca, 0 }, { 0, 0, 1 } };
                for ( int r = 0; r < 3; ++r )
                {
                    for ( int s = 0; s < 3; ++s )
                        X.R[r][s] = fabs( R[r][s] ) < 1e-12 ? 0.0 : R[r][s];
                    X.t[r] = 0.0;
                }
                double pmin[3], pmax[3];
                FrameBounds( p, &X, frame, pmin, pmax );
                const double ex = fabs( ( pmax[0] - pmin[0] ) - qdx ), ey = fabs( ( pmax[1] - pmin[1] ) - qdy );
                if ( ex > ( qdx * 0.2 > 2.0 ? qdx * 0.2 : 2.0 ) || ey > ( qdy * 0.2 > 2.0 ? qdy * 0.2 : 2.0 ) )
                    continue;
                const double err = ex + ey;
                const double tr = X.R[0][0] + X.R[1][1] + X.R[2][2];
                if ( !found || err < bestErr - 0.01 || ( fabs( err - bestErr ) <= 0.01 && tr > bestTrace + 1e-6 ) )
                {
                    // footprint centre onto centre in the target's frame; heights: a brush as
                    // tall as the old one sits exactly on it, else the paste's top meets its top
                    const bool sameHeight = fabs( ( pmax[2] - pmin[2] ) - qdz ) < 0.5;
                    const double dl[3] = { 0.5 * ( qmin[0] + qmax[0] ) - 0.5 * ( pmin[0] + pmax[0] ),
                                           0.5 * ( qmin[1] + qmax[1] ) - 0.5 * ( pmin[1] + pmax[1] ),
                                           qmax[2] - ( sameHeight ? pmax[2] : clipTop ) };
                    const double cf = cos( frame ), sf = sin( frame );
                    X.t[0] = cf * dl[0] - sf * dl[1];
                    X.t[1] = sf * dl[0] + cf * dl[1];
                    X.t[2] = dl[2];
                    *best     = X;
                    bestErr   = err;
                    bestTrace = tr;
                    found     = true;
                }
            }
        }
        return found;
    }

    // Best transform from the clipboard onto the target; false when nothing corresponds.
    bool MatchGroup( const ppMembers_t &clip, const ppMembers_t &target, ppXform_t *best, int *bestScore )
    {
        std::vector<ppXform_t> cand;
        for ( size_t t = 0; t < target.poses.size(); ++t )
            for ( size_t c = 0; c < clip.poses.size(); ++c )
                if ( !target.poses[t].modelName.empty() && clip.poses[c].modelName == target.poses[t].modelName )
                {
                    ppXform_t X;
                    PoseXform( clip.poses[c], target.poses[t], &X );
                    cand.push_back( X );
                }

        // the most telling target brushes first: big, many-vertexed
        std::vector<size_t> order;
        for ( size_t t = 0; t < target.shapes.size(); ++t )
            order.push_back( t );
        std::sort( order.begin(), order.end(), [&]( size_t a, size_t b )
        {
            const ppShape_t &A = target.shapes[a], &B = target.shapes[b];
            if ( A.radii.size() != B.radii.size() )
                return A.radii.size() > B.radii.size();
            return A.volume > B.volume;
        } );
        const size_t tries = order.size() < 6 ? order.size() : 6;
        for ( size_t o = 0; o < tries; ++o )
        {
            const ppShape_t &q = target.shapes[order[o]];
            for ( size_t c = 0; c < clip.shapes.size(); ++c )
                if ( SameShape( clip.shapes[c], q ) )
                    ShapeCandidates( clip.shapes[c], q, cand );
            if ( cand.size() > 4096 )
                break;
        }

        *bestScore = 0;
        double bestTrace = -10.0;
        for ( size_t i = 0; i < cand.size(); ++i )
        {
            const int s = Score( cand[i], clip, target );
            const double tr = cand[i].R[0][0] + cand[i].R[1][1] + cand[i].R[2][2];
            // ties (a lone symmetric brush): the smallest turn
            if ( s > *bestScore || ( s == *bestScore && s > 0 && tr > bestTrace + 1e-6 ) )
            {
                *best = cand[i];
                *bestScore = s;
                bestTrace = tr;
            }
        }
        return *bestScore > 0;
    }

    // Deepest group path both are under ("" when none).
    std::string CommonPath( const std::string &a, const std::string &b )
    {
        size_t n = 0;
        while ( n < a.size() && n < b.size() && a[n] == b[n] )
            ++n;
        if ( n == a.size() && n == b.size() )
            return a;
        if ( ( n == a.size() && b[n] == '/' ) || ( n == b.size() && a[n] == '/' ) )
            return a.substr( 0, n );
        const size_t cut = a.rfind( '/', n ? n - 1 : 0 );
        return ( cut == std::string::npos || n == 0 ) ? std::string() : a.substr( 0, cut );
    }

    std::string NodePath( selbrush_t *b )
    {
        return ( b && b->owner && b->owner != world_entity ) ? KiwiOutliner_GroupPathOf( b->owner ) : std::string();
    }

    struct ppUnit_t
    {
        std::string               path;     // "" = the ungrouped part of the selection
        std::vector<selbrush_t *> nodes;
    };

    // One unit per outermost selected group; every ungrouped pick is a unit of its own
    // (KIWI 2026-10-19, user: "paste these enhanced stair steps on top of the old steps" -
    // a row of loose steps must each get a copy, not share one).
    void UnitsOfSelection( std::vector<ppUnit_t> &units )
    {
        std::vector<selbrush_t *> nodes;
        std::vector<std::string>  paths;
        for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
        {
            nodes.push_back( b );
            paths.push_back( NodePath( b ) );
        }
        for ( size_t i = 0; i < nodes.size(); ++i )
        {
            // the outermost selected path this one sits under
            std::string root = paths[i];
            for ( size_t j = 0; j < paths.size() && !root.empty(); ++j )
            {
                const std::string &P = paths[j];
                if ( !P.empty() && P.size() < root.size() && root.compare( 0, P.size(), P ) == 0 && root[P.size()] == '/' )
                    root = P;
            }
            size_t u = 0;
            while ( u < units.size() && ( root.empty() || units[u].path != root ) )
                ++u;
            if ( u == units.size() )
            {
                units.push_back( ppUnit_t() );
                units.back().path = root;
            }
            units[u].nodes.push_back( nodes[i] );
        }
    }

    void ApplyXformToSelection( const ppXform_t &X )
    {
        // pivot at the paste's own middle keeps the float planepts precise
        double c[3] = { 0, 0, 0 };
        int n = 0;
        for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
            if ( b->def )
            {
                for ( int k = 0; k < 3; ++k )
                    c[k] += 0.5 * ( (double)b->def->mins[k] + b->def->maxs[k] );
                ++n;
            }
        if ( !n )
            return;
        for ( int k = 0; k < 3; ++k )
            c[k] /= n;

        // the paste is an exact copy: textures follow the geometry whatever the lock prefs say
        const int texLock = g_PrefsDlg->m_bTextureLock, rotLock = g_PrefsDlg->m_bRotateLock;
        const int lmLock = g_PrefsDlg->m_bLightmapLock;
        g_PrefsDlg->m_bTextureLock = g_PrefsDlg->m_bRotateLock = g_PrefsDlg->m_bLightmapLock = 1;

        const double tr = X.R[0][0] + X.R[1][1] + X.R[2][2];
        if ( tr < 3.0 - 1e-9 )
        {
            // orientation block: pivot, then the axis ROWS (v' = pivot + (v - pivot) * M, M = R^T)
            float orient[4][3];
            for ( int k = 0; k < 3; ++k )
                orient[0][k] = (float)c[k];
            for ( int a = 0; a < 3; ++a )
                for ( int b = 0; b < 3; ++b )
                    orient[1 + a][b] = (float)X.R[b][a];
            Select_ApplyMatrix_SelectedBrushes( 0, orient[0], 1.0f, 0 );   // deg != 0: models turn too
        }
        double to[3];
        XformPoint( X, c, to );
        const float move[3] = { (float)( to[0] - c[0] ), (float)( to[1] - c[1] ), (float)( to[2] - c[2] ) };
        if ( move[0] != 0.0f || move[1] != 0.0f || move[2] != 0.0f )
            for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
                Brush_Move( move, b->def, 0 );

        g_PrefsDlg->m_bTextureLock = texLock;
        g_PrefsDlg->m_bRotateLock  = rotLock;
        g_PrefsDlg->m_bLightmapLock = lmLock;
    }

    void PasteOverGroups()
    {
        std::vector<ppUnit_t> units;
        UnitsOfSelection( units );
        if ( units.empty() )
            return;

        Undo_ClearRedo();
        Undo_GeneralStart( "paste group in place" );     // stores the POINTER - literals only

        // the targets go into the record before anything changes; each owner once
        Undo_AddBrushList( &selected_brushes );
        {
            std::vector<entity_s *> owners;
            for ( selbrush_t *i = selected_brushes.next; i != &selected_brushes; i = i->next )
                if ( i->owner && i->owner != world_entity
                  && std::find( owners.begin(), owners.end(), i->owner ) == owners.end() )
                {
                    owners.push_back( i->owner );
                    Undo_AddEntity_W( (entity_s *)i->owner->def );
                }
        }

        std::vector<selbrush_t *> created, doomed;
        int replaced = 0, unmatched = 0, partial = 0, closeSize = 0;
        for ( size_t u = 0; u < units.size(); ++u )
        {
            ppMembers_t target;
            MembersOf( units[u].nodes, target );

            g_kiwiImportInCallerUndo = true;             // the paste stamps OUR record
            RadiantClipboard_Paste();                    // deselects, pastes, selects the paste
            g_kiwiImportInCallerUndo = false;

            std::vector<selbrush_t *> pasted;
            for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
                pasted.push_back( b );
            ppMembers_t clip;
            MembersOf( pasted, clip );

            ppXform_t X;
            int score = 0;
            if ( MatchGroup( clip, target, &X, &score ) )
            {
                if ( score < (int)( target.shapes.size() + target.poses.size() ) )
                    ++partial;
            }
            else if ( MatchCloseSize( clip, target, &X ) )
                ++closeSize;
            else
            {
                ++unmatched;                             // take this paste back out, keep the target
                Select_Delete();
                continue;
            }
            ApplyXformToSelection( X );

            // the replacement takes the replaced group's name, so it does not fold into the
            // group it was copied from
            std::string clipRoot;
            bool first = true;
            for ( size_t i = 0; i < pasted.size(); ++i )
            {
                const std::string p = NodePath( pasted[i] );
                clipRoot = first ? p : CommonPath( clipRoot, p );
                first = false;
            }
            if ( !units[u].path.empty() && !clipRoot.empty() && clipRoot != units[u].path )
            {
                std::vector<entity_s *> done;
                for ( size_t i = 0; i < pasted.size(); ++i )
                {
                    entity_s *e = pasted[i]->owner;
                    if ( !e || e == world_entity || std::find( done.begin(), done.end(), e ) != done.end() )
                        continue;
                    done.push_back( e );
                    KiwiOutliner_RekeyGroup( e, clipRoot, units[u].path );
                }
            }

            for ( size_t i = 0; i < pasted.size(); ++i )
            {
                Brush_UpdateSpecialMaterialFlag( pasted[i]->def );
                created.push_back( pasted[i] );
            }
            doomed.insert( doomed.end(), units[u].nodes.begin(), units[u].nodes.end() );
            ++replaced;
        }

        Select_Deselect( 1 );
        for ( size_t i = 0; i < doomed.size(); ++i )
            Select_Brush( doomed[i], 0, 0, 0 );
        if ( !doomed.empty() )
            Select_Delete();

        Select_Deselect( 1 );
        for ( size_t i = 0; i < created.size(); ++i )
            Select_Brush( created[i], 0, 0, 0 );
        Undo_EndBrushList( &selected_brushes );
        Undo_End();
        MarkMapModified();
        Sel_InvalidateFromLegacy();
        g_nUpdateBits = -1;

        Sys_Printf( "Paste Group In Place: replaced %i target%s with the clipboard (%i object%s).%s%s%s\n",
                    replaced, replaced == 1 ? "" : "s", (int)created.size(), created.size() == 1 ? "" : "s",
                    closeSize ? "  NOTE: no exact copy for some - lined up by the closest-sized brush (footprint centred, tops level)." : "",
                    partial ? "  NOTE: some replaced groups had pieces the clipboard does not - check them." : "",
                    unmatched ? "  NOTE: no clipboard brush or model matched some selections - those were left untouched." : "" );
    }

    void PasteInPlace()
    {
        if ( SelectionHasBrushes() )
        {
            PasteOverGroups();
            return;
        }

        // ── the targets: every selected model, by pose (the nodes die below) ─────────
        std::vector<selbrush_t *> targetNodes;
        std::vector<ppPose_t>     targets;
        for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
        {
            ppPose_t p;
            if ( PoseOf( b, &p ) )
            {
                targetNodes.push_back( b );
                targets.push_back( p );
            }
        }
        if ( targets.empty() )
        {
            Sys_Printf( "Paste Group In Place: select the model(s) to replace first.\n" );
            return;
        }

        Undo_ClearRedo();
        Undo_GeneralStart( "paste group in place" );     // stores the POINTER - literals only

        // Only the targets stay selected; save them into the record.  They are DELETED LAST,
        // and only those whose replacement really landed: the first version deleted up front,
        // so a paste that could not be lined up cost the user the wall and showed nothing.
        Select_Deselect( 1 );
        for ( size_t i = 0; i < targetNodes.size(); ++i )
            Select_Brush( targetNodes[i], 0, 0, 0 );
        Undo_AddBrushList( &selected_brushes );
        for ( selbrush_t *i = selected_brushes.next; i != &selected_brushes; i = i->next )
            Undo_AddEntity_W( (entity_s *)i->owner->def );

        std::vector<selbrush_t *> created, doomed;
        int replaced = 0, scaleMismatch = 0, noAnchor = 0, bySize = 0;
        for ( size_t t = 0; t < targets.size(); ++t )
        {
            const ppPose_t &T = targets[t];

            g_kiwiImportInCallerUndo = true;             // the paste stamps OUR record
            RadiantClipboard_Paste();                    // deselects, pastes, selects the paste
            g_kiwiImportInCallerUndo = false;

            // The anchor = the pasted model that STANDS FOR the target:
            //   1. the same `model` name (the biggest, should the group hold it twice);
            //   2. else the biggest model of the group - the wall, not one of its arms.
            selbrush_t *anchor = nullptr;
            ppPose_t    A;
            bool        anchorByName = false;
            int         pastedModels = 0;
            for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
            {
                ppPose_t p;
                if ( !PoseOf( b, &p ) )
                    continue;
                ++pastedModels;
                const bool byName = !T.modelName.empty() && p.modelName == T.modelName;
                const bool better = !anchor
                                 || ( byName && !anchorByName )
                                 || ( byName == anchorByName && p.size > A.size );
                if ( better )
                {
                    anchor = b;
                    A = p;
                    anchorByName = byName;
                }
            }
            if ( !anchor )
            {
                // Nothing to line up with: take this paste back out and KEEP the target.
                ++noAnchor;
                Select_Delete();
                continue;
            }
            if ( !anchorByName )
                ++bySize;                                // reported once, in the summary line
            (void)pastedModels;
            doomed.push_back( targetNodes[t] );
            {
                // rotation M (row-vector convention, v' = v * M) with  Aaxis * M = Taxis
                float ax[3][3], tx[3][3];
                AnglesAxis( A.angles, ax );
                AnglesAxis( T.angles, tx );
                float rot_around[4][3];
                for ( int k = 0; k < 3; ++k )
                    rot_around[0][k] = A.origin[k];      // pivot = the anchor's origin
                bool identity = true;
                for ( int a = 0; a < 3; ++a )
                    for ( int b = 0; b < 3; ++b )
                    {
                        float m = 0.0f;
                        for ( int k = 0; k < 3; ++k )
                            m += ax[k][a] * tx[k][b];
                        rot_around[1 + a][b] = m;
                        if ( fabsf( m - ( a == b ? 1.0f : 0.0f ) ) > 1.0e-6f )
                            identity = false;
                    }
                if ( !identity )
                    Select_ApplyMatrix_SelectedBrushes( 0, rot_around[0], 1.0f, 0 );   // deg != 0: fixed-size entities turn too

                // then carry the anchor's (unmoved: it was the pivot) origin onto the target's
                ppPose_t now;
                const float *from = PoseOf( anchor, &now ) ? now.origin : A.origin;
                const float move[3] = { T.origin[0] - from[0], T.origin[1] - from[1], T.origin[2] - from[2] };
                if ( move[0] != 0.0f || move[1] != 0.0f || move[2] != 0.0f )
                    for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
                        Brush_Move( move, b->def, 0 );

                // the replacement wall is EXACTLY the old one: reuse its angle string
                entity_s_def *adef = anchor->owner ? (entity_s_def *)anchor->owner->def : nullptr;
                if ( adef && !T.angleText.empty() )
                {
                    SetKeyValue( adef, "angles", T.angleText.c_str() );
                    ++anchor->def->version;
                }
                if ( fabsf( A.scale - T.scale ) > 1.0e-4f )
                    ++scaleMismatch;
                ++replaced;
            }
            for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
            {
                Brush_UpdateSpecialMaterialFlag( b->def );
                created.push_back( b );
            }
        }

        // now the replaced targets go (saved in the record above)
        Select_Deselect( 1 );
        for ( size_t i = 0; i < doomed.size(); ++i )
            Select_Brush( doomed[i], 0, 0, 0 );
        if ( !doomed.empty() )
            Select_Delete();
        targetNodes.clear();

        // every pasted object selected, stamped, record closed
        Select_Deselect( 1 );
        for ( size_t i = 0; i < created.size(); ++i )
            Select_Brush( created[i], 0, 0, 0 );
        Undo_EndBrushList( &selected_brushes );
        Undo_End();
        MarkMapModified();
        Sel_InvalidateFromLegacy();
        g_nUpdateBits = -1;

        Sys_Printf( "Paste Group In Place: replaced %i model%s with the clipboard group (%i object%s).%s%s%s\n",
                    replaced, replaced == 1 ? "" : "s", (int)created.size(), created.size() == 1 ? "" : "s",
                    bySize ? "  NOTE: no model name in the clipboard matched some targets - the group's biggest model was lined up instead." : "",
                    scaleMismatch ? "  NOTE: some targets had a different modelscale; the clipboard's scale was kept." : "",
                    noAnchor ? "  NOTE: the clipboard holds no model to line up with - those targets were left untouched." : "" );
    }
}

bool KiwiPastePlace_CanExecute()
{
    return RadiantClipboard_HasMapText() && ( SelectedModelCount() > 0 || SelectionHasBrushes() );
}

void KiwiPastePlace_RegisterCommands()
{
    Radiant_RegisterCommand( "KiwiPasteInPlace", 0, 0, KIWI_CMD_PASTE_IN_PLACE );
}

bool KiwiPastePlace_DispatchInstant( unsigned int cmdId )
{
    if ( cmdId != (unsigned int)KIWI_CMD_PASTE_IN_PLACE )
        return false;
    if ( KiwiCmd_Active() )
    {
        Sys_Printf( "Paste Group In Place: finish the current command first.\n" );
        return true;
    }
    if ( !RadiantClipboard_HasMapText() )
    {
        Sys_Printf( "Paste Group In Place: copy the group (the model and what belongs to it) first.\n" );
        return true;
    }
    PasteInPlace();
    return true;
}

void KiwiPastePlace_BuildMenu( void *frameMenu )
{
    HMENU menu = (HMENU)frameMenu;
    HMENU edit = menu ? ::GetSubMenu( menu, 1 ) : nullptr;
    if ( !edit || ::GetMenuState( edit, KIWI_CMD_PASTE_IN_PLACE, MF_BYCOMMAND ) != 0xFFFFFFFFu )
        return;
    ::AppendMenuA( edit, MF_STRING, KIWI_CMD_PASTE_IN_PLACE, "Paste Group In Place (replace selected models)\tCtrl+Shift+V" );
}

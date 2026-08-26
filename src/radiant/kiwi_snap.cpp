#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Snap resolution and public contracts are documented in kiwi_snap.h.
// Brush/patch candidates use Pick(); construction geometry is scanned locally.
// Queries are read-only.
#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"        // camera_s
#include "prefs.h"          // g_PrefsDlg (camera_fov — the screen-scaled marker)
#include <imgui/imgui.h>

#include "kiwi_camera.h"    // shared ortho-aware screen scale
#include "kiwi_command.h"   // active command and cursor
#include "kiwi_snap.h"
#include "kiwi_conselect.h" // construction self-snap exclusion
#include "kiwi_construct.h"
#include "kiwi_transform.h" // pivot placement and snap-dot state
#include "kiwi_grid.h"
#include "kiwi_region.h"      // KREG_JOIN_DIST weld tolerance
#include "kiwi_lines.h"
#include "kiwi_units.h"
#include "radiant_registry.h"
#include "kiwi_vec.h"         // Dot3/Sub3/Cross3/Norm3

#include <math.h>
#include <stdio.h>

// ── ported entry points (verified against their definitions) ────────────────
extern camera_s *Ed_Camera();             // camwnd.cpp
// Active-list sentinel at 0x23F189C. Read only while gathering brush-edge
// directions incident to the drawing-tool anchor.
extern selbrush_t active_brushes;

namespace
{
    const char *KSNAP_SECTION = "KiwiUX";
    int         s_showMarkers = -1;        // -1 = not loaded yet
    // One near-black marker color. Labels carry type colors; kiwi_lines has no
    // alpha, so dark gray remains legible on dark geometry.
    const float KSNAP_COL_MARK[3] = { 0.05f, 0.05f, 0.07f };
    // Filled-dot and separated-ring radii in SCREEN PIXELS.
    const float KSNAP_DOT_PIX   = 2.0f;
    const float KSNAP_RING_PIX  = 6.0f;
    // Hovered-face accents are smaller than committed snap markers.
    // KSNAP_ACCENT_MAX bounds the advertised points and line-batch cost.
    const float KSNAP_ACCENT_PIX  = 1.5f;
    const int   KSNAP_ACCENT_SEGS = 6;
    const int   KSNAP_ACCENT_MAX  = 40;    // maximum advertised face targets
    const int   KSNAP_DOT_SEGS  = 6;       // hexagon — reads as solid at 2 px
    const int   KSNAP_RING_SEGS = 12;      // 12 chords read as round at 6 px

    int   s_labelX = 0;
    int   s_labelY = 0;
    // Unit direction used to align edge and midpoint marker ticks.
    float s_edgeDir[3] = { 0.0f, 0.0f, 0.0f };
    bool  s_haveEdgeDir = false;
    // Base axis capture radius in SCREEN PIXELS.
    const float KSNAP_AXIS_PIXELS      = 10.0f;   // base capture radius
    // Widen every axis to 30 SCREEN PIXELS near a flat camera, where projected
    // guides converge and are hardest to aim at.
    const float KSNAP_AXIS_PIXELS_FLAT = 30.0f;
    // Pitch ramp on |vpn.z|: flat at 0.35, base radius restored at 0.70.
    const float KSNAP_AXIS_FLAT_LOW    = 0.35f;
    const float KSNAP_AXIS_FLAT_HIGH   = 0.70f;
    // Guide half-length in SCREEN PIXELS at the anchor depth.
    const float KSNAP_AXIS_GUIDE_PIX   = 420.0f;
    // Reject projected axes shorter than 8 SCREEN PIXELS: they read as dots.
    const float KSNAP_AXIS_MIN_PROJ    = 8.0f;
    // Dash pitch is a guide fraction because the world half-length is clamped.
    // Thirty-two dashes per side cover the full guide at every zoom.
    const int   KSNAP_AXIS_DASHES      = 32;
    const float KSNAP_AXIS_DASH_DUTY   = 0.55f;   // ink : pitch
    const int   KSNAP_AXIS_MAX_SEGS    = 96;      // 2*32 dashes + the anchor dot
    // Dim gray substitutes for alpha, which kiwi_lines does not support.
    const float KSNAP_COL_AXIS[3]      = { 0.42f, 0.42f, 0.46f };

    // Latched by the query for the guide pass and the label.
    bool        s_axisHave      = false;
    float       s_axisAnchor[3] = { 0.0f, 0.0f, 0.0f };
    float       s_axisDir[3]    = { 0.0f, 0.0f, 1.0f };
    float       s_axisHalfLen   = 0.0f;
    const char *s_axisName      = "Z";
    // Incident edge directions join the ordinary axis set. Dedup parallel lines
    // and keep at most three directions closest to the cursor aim.
    enum { KSNAP_MAX_EXT_AXES = 3 };
    // Free-tool angle snapping is a capture band, not a quantizer. This is the
    // allowed lateral miss in SCREEN PIXELS at the segment's depth.
    const float KSNAP_ANGLE_BAND_PIX = 6.0f;
    // Floor the band at 1 DEGREE so exact stops remain reachable when zoomed in.
    const float KSNAP_ANGLE_BAND_MIN = 1.0f;
    // Cap the band at 0.22 of a 15-degree step so short or distant segments
    // never turn the capture band into full quantization.
    const float KSNAP_ANGLE_MAX_FRAC = 0.22f;
    // Latched absolute angle for the marker label.
    float s_angleDeg = 0.0f;
    // A planar anchor on an existing line reports its stop relative to that line.
    bool  s_angleIsRel = false;
    float s_angleRel   = 0.0f;
    // Shared ortho-aware WORLD UNITS per SCREEN PIXEL scale.
    inline float WorldPerPixel( const camera_s *c, const float *world )
    {
        (void)c;
        return KiwiCam_WorldPerPixel( world );
    }
    // Pull markers 0.25 WORLD UNITS toward the camera to avoid depth fighting.
    void Nudge( const camera_s *c, const float *in, float *out )
    {
        out[0] = in[0] - c->vpn[0] * 0.25f;
        out[1] = in[1] - c->vpn[1] * 0.25f;
        out[2] = in[2] - c->vpn[2] * 0.25f;
    }

    void AddNudged( const camera_s *c, const float *a, const float *b )
    {
        float na[3], nb[3];
        Nudge( c, a, na );
        Nudge( c, b, nb );
        KiwiLines_Add( na, nb );
    }
    // Camera-facing polygon of world radius r. Long diagonals make the tiny solid
    // form read as filled despite the line-only renderer.
    void EmitDisc( const camera_s *c, const float *p, float r, int segs, bool solid )
    {
        float prev[3], pt[3];
        for ( int i = 0; i <= segs; ++i )
        {
            const float th = ( 6.283185307179586f * (float)( i % segs ) ) / (float)segs;
            const float cx = cosf( th ) * r;
            const float cy = sinf( th ) * r;
            for ( int k = 0; k < 3; ++k )
                pt[k] = p[k] + c->vright[k] * cx + c->vup[k] * cy;
            if ( i > 0 )
                AddNudged( c, prev, pt );
            prev[0] = pt[0]; prev[1] = pt[1]; prev[2] = pt[2];
        }
        if ( !solid )
            return;
        // Long diagonals visually fill the small polygon.
        for ( int i = 0; i < segs / 2; ++i )
        {
            const float th = ( 6.283185307179586f * (float)i ) / (float)segs;
            const float cx = cosf( th ) * r;
            const float cy = sinf( th ) * r;
            float a[3], b[3];
            for ( int k = 0; k < 3; ++k )
            {
                a[k] = p[k] + c->vright[k] * cx + c->vup[k] * cy;
                b[k] = p[k] - c->vright[k] * cx - c->vup[k] * cy;
            }
            AddNudged( c, a, b );
        }
    }
    // Point-rank glyph: filled dot plus separated ring.
    void EmitDotAndRing( const camera_s *c, const float *p, float wpp )
    {
        EmitDisc( c, p, KSNAP_DOT_PIX  * wpp, KSNAP_DOT_SEGS,  true  );
        EmitDisc( c, p, KSNAP_RING_PIX * wpp, KSNAP_RING_SEGS, false );
    }
    // Ray intersection with Z=0. |dir.z| below 1e-5 is parallel; intersections
    // farther than 1e6 WORLD UNITS or behind the origin are refused.
    bool RayHitsGroundPlane( const ray_t &ray, float *out )
    {
        const float dz = ray.dir[2];
        if ( dz > -1e-5f && dz < 1e-5f )
            return false;
        const float t = -ray.origin[2] / dz;
        if ( !( t > 0.0f ) || t > 1.0e6f )
            return false;
        out[0] = ray.origin[0] + ray.dir[0] * t;
        out[1] = ray.origin[1] + ray.dir[1] * t;
        out[2] = 0.0f;
        // Clamp to the same finite KCON_PLANE_REACH window the ground grid draws,
        // centered on the camera so grazing rays cannot land beyond the visible lattice.
        if ( const camera_s *c = Ed_Camera() )
        {
            for ( int k = 0; k < 2; ++k )
            {
                const float lo = c->origin[k] - KCON_PLANE_REACH;
                const float hi = c->origin[k] + KCON_PLANE_REACH;
                if ( out[k] < lo ) out[k] = lo;
                if ( out[k] > hi ) out[k] = hi;
            }
        }
        return true;
    }

    void RayPoint( const ray_t &ray, float dist, float *out )
    {
        out[0] = ray.origin[0] + ray.dir[0] * dist;
        out[1] = ray.origin[1] + ray.dir[1] * dist;
        out[2] = ray.origin[2] + ray.dir[2] * dist;
    }
    // Use the camera view axis, not the cursor ray, so the bearing plane does not
    // rock across a perspective viewport. Fall back to the ray before failing.
    bool ViewPlaneNormal( const ray_t &ray, float *out )
    {
        const camera_s *c = Ed_Camera();
        Copy3( c ? c->vpn : ray.dir, out );
        if ( Norm3( out ) )
            return true;
        Copy3( ray.dir, out );
        return Norm3( out );
    }
    // Construction geometry is outside Pick(). Each scan returns its closest
    // candidate in SCREEN PIXELS.
    struct conBest_t
    {
        bool  hit = false;
        float dist = 0.0f;             // pixels
        float pos[3] = { 0.0f, 0.0f, 0.0f };
        float dir[3] = { 0.0f, 0.0f, 0.0f };   // segment direction (edge arm only)
        bool  haveDir = false;
    };

    inline float PixelDist( float ax, float ay, float bx, float by )
    {
        const float dx = ax - bx, dy = ay - by;
        return sqrtf( dx * dx + dy * dy );
    }
    // Hidden construction is never snappable. A live construction move also mutes
    // its own object, except while placing a pivot, when no geometry is moving.
    inline bool ConCandidateUsable( const kconObject_t *o, int i )
    {
        if ( !o || o->hidden )
            return false;
        if ( KiwiXform_PivotPlacing() )
            return true;
        return !KiwiConSel_SnapMuted( i );
    }
    // Construction anchors within the 7 SCREEN-PIXEL point radius.
    void ScanConAnchors( float curX, float curY, conBest_t *best )
    {
        const int count = KiwiCon_Count();
        for ( int i = 0; i < count; ++i )
        {
            const kconObject_t *o = KiwiCon_At( i );
            if ( !ConCandidateUsable( o, i ) )
                continue;
            const int anchors = KiwiCon_AnchorCount( *o );
            for ( int a = 0; a < anchors; ++a )
            {
                float w[3], px, py;
                if ( !KiwiCon_AnchorWorld( *o, a, w ) )
                    break;
                if ( !Pick_WorldToImage( w, &px, &py ) )   // false = behind the eye
                    continue;
                const float d = PixelDist( px, py, curX, curY );
                if ( d > KSNAP_R_CON_POINT )
                    continue;
                if ( best->hit && d >= best->dist )
                    continue;
                best->hit  = true;
                best->dist = d;
                best->pos[0] = w[0]; best->pos[1] = w[1]; best->pos[2] = w[2];
            }
        }
    }
    // Closest point on a construction segment within 8 SCREEN PIXELS. Compute the
    // parameter in screen space, then apply it to the world segment.
    void ScanConSegments( float curX, float curY, conBest_t *best )
    {
        const int count = KiwiCon_Count();
        for ( int i = 0; i < count; ++i )
        {
            const kconObject_t *o = KiwiCon_At( i );
            if ( !ConCandidateUsable( o, i ) )
                continue;
            const int segs = KiwiCon_SegmentCount( *o );
            for ( int s = 0; s < segs; ++s )
            {
                float wa[3], wb[3];
                if ( !KiwiCon_SegmentWorld( *o, s, wa, wb ) )
                    break;
                float ax, ay, bx, by;
                if ( !Pick_WorldToImage( wa, &ax, &ay ) || !Pick_WorldToImage( wb, &bx, &by ) )
                    continue;                 // either end behind the eye — skip whole
                float       t = 0.0f;
                const float d = Pick_SegDist2D( curX, curY, ax, ay, bx, by, &t );
                // Selection uses a 10 px click box; snapping deliberately uses 8 px.
                if ( d > KSNAP_R_CON_SEG )
                    continue;
                if ( best->hit && d >= best->dist )
                    continue;
                best->hit  = true;
                best->dist = d;
                for ( int k = 0; k < 3; ++k )
                {
                    best->pos[k] = wa[k] + ( wb[k] - wa[k] ) * t;
                    best->dir[k] = wb[k] - wa[k];
                }
                const float l = sqrtf( Dot3( best->dir, best->dir ) );
                best->haveDir = ( l > 1.0e-4f );
                if ( best->haveDir )
                    for ( int k = 0; k < 3; ++k )
                        best->dir[k] /= l;
            }
        }
    }
    // Line-rank axis candidates through the drawing-tool anchor.
    struct axisCand_t
    {
        float       dir[3];
        const char *name;
    };
    // Prefer X/Y/Z labels for world-aligned directions; otherwise keep the
    // candidate's plane or extension label.
    const char *AxisName( const float *d, const char *fallback )
    {
        if ( fabsf( d[0] ) > 0.999f ) return "X";
        if ( fabsf( d[1] ) > 0.999f ) return "Y";
        if ( fabsf( d[2] ) > 0.999f ) return "Z";
        return fallback;
    }
    // Capture radius in SCREEN PIXELS, ramped from 30 at a flat camera to 10
    // at |vpn.z| >= 0.70. The rule currently applies to every axis candidate.
    float AxisRadius( const camera_s *c, const axisCand_t &a )
    {
        (void)a;
        const float up = fabsf( c->vpn[2] );          // 0 = at the horizon, 1 = at a pole
        if ( up <= KSNAP_AXIS_FLAT_LOW )
            return KSNAP_AXIS_PIXELS_FLAT;
        if ( up >= KSNAP_AXIS_FLAT_HIGH )
            return KSNAP_AXIS_PIXELS;
        const float t = ( up - KSNAP_AXIS_FLAT_LOW )
                      / ( KSNAP_AXIS_FLAT_HIGH - KSNAP_AXIS_FLAT_LOW );
        return KSNAP_AXIS_PIXELS_FLAT
             + ( KSNAP_AXIS_PIXELS - KSNAP_AXIS_PIXELS_FLAT ) * t;
    }
    // Candidate order is world Z, plane U/V/N, then extension directions.
    // Parallel duplicates are dropped; fixed axes own an otherwise dead tie.
    enum { KSNAP_AXIS_CANDS = 4 + KSNAP_MAX_EXT_AXES };
    // extDirs contains extCount unit directions, already ranked by the caller.
    int GatherAxisCandidates( const kconPlane_t &plane, const float *extDirs,
                              int extCount, axisCand_t *out )
    {
        int n = 0;
        out[n].dir[0] = 0.0f; out[n].dir[1] = 0.0f; out[n].dir[2] = 1.0f;
        out[n].name = "Z";
        ++n;

        const float *pa[3] = { plane.u, plane.v, plane.normal };
        const char  *pn[3] = { "U", "V", "N" };
        for ( int i = 0; i < 3 && n < KSNAP_AXIS_CANDS; ++i )
        {
            const float len = sqrtf( Dot3( pa[i], pa[i] ) );
            if ( !( len > 1.0e-4f ) )
                continue;
            float d[3] = { pa[i][0] / len, pa[i][1] / len, pa[i][2] / len };
            if ( fabsf( d[2] ) > 0.999f )
                continue;                    // duplicates world Z
            out[n].dir[0] = d[0]; out[n].dir[1] = d[1]; out[n].dir[2] = d[2];
            out[n].name = AxisName( d, pn[i] );
            ++n;
        }
        // Append extension axes last; pixel distance still selects the winner.
        for ( int i = 0; i < extCount && n < KSNAP_AXIS_CANDS; ++i )
        {
            const float *d = extDirs + (size_t)i * 3;
            bool dup = false;
            for ( int k = 0; k < n && !dup; ++k )
                dup = ( fabsf( Dot3( d, out[k].dir ) ) > 0.999f );
            if ( dup )
                continue;
            Copy3( d, out[n].dir );
            // An extension that happens to lie along a world axis is named for the
            // axis (AxisName), so the label never says "ext" about the vertical.
            out[n].name = AxisName( d, "ext" );
            ++n;
        }
        return n;
    }
    // World-aligned axes quantize the ABSOLUTE world coordinate; slanted axes
    // quantize signed distance from the anchor. No grid spacing leaves along unchanged.
    float SnapAlongAxis( const float *anchor, const float *dir, float along )
    {
        const float g = KiwiUnits_GridSpacingWorld();
        if ( !( g > 0.0f ) )
            return along;
        int wax = -1;
        for ( int k = 0; k < 3; ++k )
            if ( fabsf( dir[k] ) > 0.999f )
                wax = k;
        if ( wax >= 0 )
        {
            const float posw = anchor[wax] + dir[wax] * along;
            const float snap = floorf( posw / g + 0.5f ) * g;
            return ( snap - anchor[wax] ) / dir[wax];
        }
        return floorf( along / g + 0.5f ) * g;
    }
    // Screen-space closest point on an axis segment; apply the same parameter to
    // the world segment so its pixel distance is comparable with other line snaps.
    struct axisBest_t
    {
        bool        hit  = false;
        float       dist = 0.0f;               // pixels
        float       pos[3] = { 0.0f, 0.0f, 0.0f };
        float       dir[3] = { 0.0f, 0.0f, 1.0f };
        float       halfLen = 0.0f;
        const char *name = "Z";
    };

    void ScanAxes( const camera_s *c, const float *anchor, const kconPlane_t &plane,
                   const float *extDirs, int extCount,
                   float curX, float curY, axisBest_t *best )
    {
        const float wpp = WorldPerPixel( c, anchor );
        if ( !( wpp > 0.0f ) )
            return;
        float half = KSNAP_AXIS_GUIDE_PIX * wpp;
        if ( half < 32.0f )    half = 32.0f;
        if ( half > 32768.0f ) half = 32768.0f;

        axisCand_t cand[KSNAP_AXIS_CANDS];
        const int n = GatherAxisCandidates( plane, extDirs, extCount, cand );

        for ( int i = 0; i < n; ++i )
        {
            // The guide half-length is shortened until BOTH ends project — an axis
            // through a point near the eye plane runs behind the camera at full
            // length, and Pick_WorldToImage refuses those (as it must).
            float ax, ay, bx, by;
            float use = half;
            bool  ok  = false;
            for ( int tryI = 0; tryI < 3 && !ok; ++tryI )
            {
                const float wa[3] = { anchor[0] - cand[i].dir[0] * use,
                                      anchor[1] - cand[i].dir[1] * use,
                                      anchor[2] - cand[i].dir[2] * use };
                const float wb[3] = { anchor[0] + cand[i].dir[0] * use,
                                      anchor[1] + cand[i].dir[1] * use,
                                      anchor[2] + cand[i].dir[2] * use };
                ok = Pick_WorldToImage( wa, &ax, &ay ) && Pick_WorldToImage( wb, &bx, &by );
                if ( !ok )
                    use *= 0.25f;
            }
            if ( !ok )
                continue;

            const float ex = bx - ax, ey = by - ay;
            const float len2 = ex * ex + ey * ey;
            if ( len2 < KSNAP_AXIS_MIN_PROJ * KSNAP_AXIS_MIN_PROJ )
                continue;                    // edge-on: it is a dot, not a line
            float       t = 0.0f;
            const float d = Pick_SegDist2D( curX, curY, ax, ay, bx, by, &t );
            if ( d > AxisRadius( c, cand[i] ) )
                continue;
            if ( best->hit && d >= best->dist )
                continue;
            // Grid-quantize along the axis using the absolute-vs-distance rule above.
            const float along = SnapAlongAxis( anchor, cand[i].dir,
                                               -use + ( 2.0f * use ) * t );

            best->hit     = true;
            best->dist    = d;
            best->halfLen = use;
            best->name    = cand[i].name;
            for ( int k = 0; k < 3; ++k )
            {
                best->dir[k] = cand[i].dir[k];
                best->pos[k] = anchor[k] + cand[i].dir[k] * along;
            }
        }
    }
    // Construction midpoints use the 7 SCREEN-PIXEL point radius and outrank
    // brush midpoints and all line snaps.
    void ScanConMidpoints( float curX, float curY, conBest_t *best )
    {
        const int count = KiwiCon_Count();
        for ( int i = 0; i < count; ++i )
        {
            const kconObject_t *o = KiwiCon_At( i );
            if ( !ConCandidateUsable( o, i ) )
                continue;
            const int segs = KiwiCon_SegmentCount( *o );
            for ( int s = 0; s < segs; ++s )
            {
                float wa[3], wb[3];
                if ( !KiwiCon_SegmentWorld( *o, s, wa, wb ) )
                    break;
                const float mid[3] = { ( wa[0] + wb[0] ) * 0.5f,
                                       ( wa[1] + wb[1] ) * 0.5f,
                                       ( wa[2] + wb[2] ) * 0.5f };
                float px, py;
                if ( !Pick_WorldToImage( mid, &px, &py ) )
                    continue;
                const float d = PixelDist( px, py, curX, curY );
                if ( d > KSNAP_R_CON_POINT )
                    continue;
                if ( best->hit && d >= best->dist )
                    continue;
                best->hit  = true;
                best->dist = d;
                for ( int k = 0; k < 3; ++k )
                {
                    best->pos[k] = mid[k];
                    best->dir[k] = wb[k] - wa[k];
                }
                const float l = sqrtf( Dot3( best->dir, best->dir ) );
                best->haveDir = ( l > 1.0e-4f );
                if ( best->haveDir )
                    for ( int k = 0; k < 3; ++k )
                        best->dir[k] /= l;
            }
        }
    }
    // Construction crossings use 3D closest approach and accept gaps up to
    // KCON_ISECT_DIST (0.25 WORLD UNITS), reporting the closest-points midpoint.
    // Reject adjacent same-object segments (including closed seams), parallel or
    // collinear pairs, and shared endpoints. Non-adjacent self-crossings remain valid.
    void ScanConIntersections( float curX, float curY, conBest_t *best )
    {
        // Gather bounded world segments with owner/ordinal data for adjacency checks.
        struct seg3_t { float a[3], b[3]; int obj, ord; bool wrapEnd; };
        seg3_t segs[KSNAP_MAX_CSEGS];
        int    segCount = 0;

        const int count = KiwiCon_Count();
        for ( int i = 0; i < count && segCount < KSNAP_MAX_CSEGS; ++i )
        {
            const kconObject_t *o = KiwiCon_At( i );
            if ( !ConCandidateUsable( o, i ) )
                continue;
            const int n = KiwiCon_SegmentCount( *o );
            for ( int s = 0; s < n && segCount < KSNAP_MAX_CSEGS; ++s )
            {
                if ( !KiwiCon_SegmentWorld( *o, s, segs[segCount].a, segs[segCount].b ) )
                    break;
                segs[segCount].obj = i;
                segs[segCount].ord = s;
                // A CLOSED object's last segment is adjacent to its first — the wrap
                // is implicit in the store (kiwi_construct.h), so it has to be made
                // explicit here or the seam vertex becomes a fake intersection.
                segs[segCount].wrapEnd = ( s == n - 1 ) && ( n > 2 )
                                      && ( ( o->type == KCON_CIRCLE ) || ( o->type == KCON_RECT )
                                           || o->closed );
                ++segCount;
            }
        }

        for ( int i = 0; i < segCount; ++i )
        {
            for ( int j = i + 1; j < segCount; ++j )
            {
                // Adjacent segments of one object meet at a vertex, not an intersection.
                if ( segs[i].obj == segs[j].obj )
                {
                    const int da = segs[j].ord - segs[i].ord;      // j > i, so da > 0
                    if ( da <= 1 )
                        continue;
                    // …and the seam pair of a closed object, which is adjacent by
                    // wrap rather than by ordinal.
                    if ( segs[j].wrapEnd && segs[i].ord == 0 )
                        continue;
                }

                float d1[3], d2[3], cr[3];
                Sub3( segs[i].b, segs[i].a, d1 );
                Sub3( segs[j].b, segs[j].a, d2 );
                Cross3( d1, d2, cr );
                const float l1 = sqrtf( Dot3( d1, d1 ) );
                const float l2 = sqrtf( Dot3( d2, d2 ) );
                if ( !( l1 > 1.0e-4f ) || !( l2 > 1.0e-4f ) )
                    continue;                 // a degenerate segment crosses nothing
                // |d1 x d2|/(|d1||d2|) is the angle sine. Below 1e-3 (about
                // 0.06 degrees) the crossing location is ambiguous, while real shallow crossings remain.
                if ( sqrtf( Dot3( cr, cr ) ) / ( l1 * l2 ) < 1.0e-3f )
                    continue;

                float w[3], px, py;
                const float gap = KiwiCon_SegSegClosest( segs[i].a, segs[i].b,
                                                         segs[j].a, segs[j].b,
                                                         0, 0, w );
                if ( gap > KCON_ISECT_DIST )
                    continue;                 // they do not meet
                // A shared endpoint already belongs to the higher-priority endpoint arm.
                // T-junctions survive because only one segment contributes an endpoint.
                {
                    const float *ea[2] = { segs[i].a, segs[i].b };
                    const float *eb[2] = { segs[j].a, segs[j].b };
                    bool shared = false;
                    for ( int u = 0; u < 2 && !shared; ++u )
                        for ( int v = 0; v < 2 && !shared; ++v )
                        {
                            float du[3], dw[3];
                            Sub3( ea[u], eb[v], du );
                            Sub3( w,     ea[u], dw );
                            shared = sqrtf( Dot3( du, du ) ) <= KCON_ISECT_DIST
                                  && sqrtf( Dot3( dw, dw ) ) <= KCON_ISECT_DIST;
                        }
                    if ( shared )
                        continue;
                }

                if ( !Pick_WorldToImage( w, &px, &py ) )
                    continue;
                const float d = PixelDist( px, py, curX, curY );
                if ( d > KSNAP_R_CON_POINT )
                    continue;
                if ( best->hit && d >= best->dist )
                    continue;
                best->hit  = true;
                best->dist = d;
                best->pos[0] = w[0]; best->pos[1] = w[1]; best->pos[2] = w[2];
            }
        }
    }
    // Brush-face center target: arithmetic mean of winding vertices. SEL_MASK_FACE
    // is required so Pick resolves faceIndex; patches have no winding and are excluded.
    bool FaceCentroid( const sel_item_t &it, float *out )
    {
        if ( !it.brush || !it.brush->def )
            return false;
        // Geometry faces, never visibility faces.
        brush_t *def = it.brush->def;
        if ( !def->faces || it.faceIndex < 0 || it.faceIndex >= def->faceCount )
            return false;
        winding_t *w = def->faces[it.faceIndex].w;
        if ( !w || w->numpoints < 3 || w->numpoints > MAX_POINTS_ON_WINDING )
            return false;
        out[0] = out[1] = out[2] = 0.0f;
        for ( int i = 0; i < w->numpoints; ++i )
            for ( int k = 0; k < 3; ++k )
                out[k] += w->p[i][k];
        for ( int k = 0; k < 3; ++k )
            out[k] /= (float)w->numpoints;
        return true;
    }
    // Squared WORLD-UNIT distance from p to segment a..b.
    float PointSegDist2( const float *p, const float *a, const float *b )
    {
        float ab[3], ap[3];
        for ( int k = 0; k < 3; ++k ) { ab[k] = b[k] - a[k]; ap[k] = p[k] - a[k]; }
        const float len2 = Dot3( ab, ab );
        float t = 0.0f;
        if ( len2 > 1.0e-8f )
        {
            t = Dot3( ap, ab ) / len2;
            if ( t < 0.0f ) t = 0.0f;
            if ( t > 1.0f ) t = 1.0f;
        }
        float d[3];
        for ( int k = 0; k < 3; ++k )
            d[k] = p[k] - ( a[k] + ab[k] * t );
        return Dot3( d, d );
    }
    // Whole-brush bounds reject first; this edge count is a pathological-map cap.
    enum { KSNAP_MAX_ANCHOR_EDGES = 4096 };
    // One incident-edge walk serves both the single relative-angle base and the
    // capped extension-axis list.
    struct anchorDir_t
    {
        float dir[3] = { 0.0f, 0.0f, 0.0f };
        float d2     = 0.0f;               // point-to-segment distance², world units²
        float key    = 0.0f;               // the sort key — see GatherAnchorDirs
    };
    // Insert by ascending key, deduplicate parallel directions, and drop the worst
    // entry at maxN. Equal keys prefer the later candidate.
    void AnchorDirPush( anchorDir_t *list, int *count, int maxN,
                        const float *dir, float d2, float key )
    {
        for ( int i = 0; i < *count; ++i )
        {
            if ( fabsf( Dot3( dir, list[i].dir ) ) <= 0.999f )
                continue;
            if ( d2 < list[i].d2 )
                list[i].d2 = d2;           // same line, a nearer sample of it
            return;
        }
        int at = *count;
        while ( at > 0 && list[at - 1].key >= key )
            --at;
        if ( at >= maxN )
            return;                        // worse than everything we are keeping
        int last = *count;
        if ( last >= maxN )
            last = maxN - 1;               // the worst entry falls off the end
        for ( int i = last; i > at; --i )
            list[i] = list[i - 1];
        Copy3( dir, list[at].dir );
        list[at].d2  = d2;
        list[at].key = key;
        if ( *count < maxN )
            ++*count;
    }
    // A line contains the anchor when point-to-segment distance is within
    // KREG_JOIN_DIST WORLD UNITS, the shared endpoint weld tolerance. With aim, rank
    // by collinearity; without aim, prefer the in-progress chain then nearest distance.
    int GatherAnchorDirs( const float anchor[3], const float *aim,
                          anchorDir_t *out, int maxN )
    {
        int count = 0;
        if ( maxN < 1 )
            return 0;
        // The in-progress segment is not stored until Finish(), so add its direction
        // explicitly. It is the unambiguous first choice for continuation and relative angle.
        {
            float prev[3];
            if ( KiwiCon_ToolPrevAnchor( prev ) )
            {
                float dir[3];
                for ( int k = 0; k < 3; ++k )
                    dir[k] = anchor[k] - prev[k];
                const float l = sqrtf( Dot3( dir, dir ) );
                if ( l > 1.0e-4f )
                {
                    for ( int k = 0; k < 3; ++k )
                        dir[k] /= l;
                    // Negative key makes the in-progress chain beat all real distance-squared keys.
                    AnchorDirPush( out, &count, maxN, dir, 0.0f,
                                   aim ? ( 1.0f - fabsf( Dot3( dir, aim ) ) ) : -1.0f );
                    if ( !aim && maxN == 1 )
                        return count;
                }
            }
        }

        const float tol = KREG_JOIN_DIST;

        // 1. construction segments
        const int conCount = KiwiCon_Count();
        for ( int i = 0; i < conCount; ++i )
        {
            const kconObject_t *o = KiwiCon_At( i );
            // Apply the same hidden/self-snap gate used by every construction scan.
            if ( !ConCandidateUsable( o, i ) )
                continue;
            const int segs = KiwiCon_SegmentCount( *o );
            for ( int s = 0; s < segs; ++s )
            {
                float wa[3], wb[3];
                if ( !KiwiCon_SegmentWorld( *o, s, wa, wb ) )
                    break;
                const float d2 = PointSegDist2( anchor, wa, wb );
                if ( d2 > tol * tol )
                    continue;
                float dir[3];
                for ( int k = 0; k < 3; ++k )
                    dir[k] = wb[k] - wa[k];
                const float l = sqrtf( Dot3( dir, dir ) );
                if ( !( l > 1.0e-4f ) )
                    continue;
                for ( int k = 0; k < 3; ++k )
                    dir[k] /= l;
                AnchorDirPush( out, &count, maxN, dir, d2,
                               aim ? ( 1.0f - fabsf( Dot3( dir, aim ) ) ) : d2 );
            }
        }
        // Brush edges: reject whole bounds expanded by KREG_JOIN_DIST before spending
        // the KSNAP_MAX_ANCHOR_EDGES budget.
        int budget = KSNAP_MAX_ANCHOR_EDGES;
        selbrush_t *lists[2] = { &active_brushes, &selected_brushes };
        for ( int L = 0; L < 2 && budget > 0; ++L )
        {
            selbrush_t *head = lists[L];
            // Sentinel walk: init from .next, advance via ->next.
            for ( selbrush_t *b = head->next; b && b != head && budget > 0; b = b->next )
            {
                if ( !Pick_BrushPickable( b ) )
                    continue;
                brush_t *def = b->def;
                if ( !def || !def->faces || b->patch )
                    continue;
                bool outside = false;
                for ( int k = 0; k < 3 && !outside; ++k )
                    outside = ( anchor[k] < def->mins[k] - tol )
                           || ( anchor[k] > def->maxs[k] + tol );
                if ( outside )
                    continue;

                for ( int f = 0; f < def->faceCount && budget > 0; ++f )
                {
                    winding_t *w = def->faces[f].w;
                    if ( !w || w->numpoints < 2 || w->numpoints > MAX_POINTS_ON_WINDING )
                        continue;
                    for ( int i = 0; i < w->numpoints && budget > 0; ++i )
                    {
                        --budget;
                        const int j = ( i + 1 ) % w->numpoints;
                        const float d2 = PointSegDist2( anchor, w->p[i], w->p[j] );
                        if ( d2 > tol * tol )
                            continue;
                        float dir[3];
                        for ( int k = 0; k < 3; ++k )
                            dir[k] = w->p[j][k] - w->p[i][k];
                        const float l = sqrtf( Dot3( dir, dir ) );
                        if ( !( l > 1.0e-4f ) )
                            continue;
                        for ( int k = 0; k < 3; ++k )
                            dir[k] /= l;
                        AnchorDirPush( out, &count, maxN, dir, d2,
                                       aim ? ( 1.0f - fabsf( Dot3( dir, aim ) ) ) : d2 );
                    }
                }
            }
        }
        return count;
    }
    // Single un-aimed incident direction used as the planar relative-angle base.
    bool AnchorLineDir( const float anchor[3], float outDir[3] )
    {
        anchorDir_t one[1];
        if ( GatherAnchorDirs( anchor, nullptr, one, 1 ) < 1 )
            return false;
        Copy3( one[0].dir, outDir );
        return true;
    }
}
// Registry-backed marker visibility.
bool KiwiSnap_ShowMarkers()
{
    if ( s_showMarkers < 0 )
        s_showMarkers = Radiant_ProfileGetInt( KSNAP_SECTION, "SnapMarkers", 1 ) ? 1 : 0;
    return s_showMarkers != 0;
}

void KiwiSnap_SetShowMarkers( bool on )
{
    const int v = on ? 1 : 0;
    if ( s_showMarkers == v )
        return;
    s_showMarkers = v;
    Radiant_ProfileSetInt( KSNAP_SECTION, "SnapMarkers", v );
}

const char *KiwiSnap_TypeName( snap_type_t t )
{
    switch ( t )
    {
    case SNAP_GRID:         return "grid";
    case SNAP_VERTEX:       return "vert";
    case SNAP_EDGE_MID:     return "mid";
    case SNAP_EDGE:         return "edge";
    case SNAP_FACE:         return "face";
    case SNAP_FACE_CENTER:  return "center";
    case SNAP_ENDPOINT:     return "end";       // construction anchor
    case SNAP_INTERSECTION: return "isect";
    case SNAP_AXIS:         return "axis";
    case SNAP_CPLANE:       return "cplane";
    case SNAP_ANGLE:        return "angle";
    default:                return "off";
    }
}
// For SNAP_FACE, intersect the target plane with the gesture axis:
//   t = normal dot (hit - ref) / normal dot axis
// Point targets retain exact projection. Near-parallel faces are refused because
// their intersection depth is unstable.
bool KiwiSnap_AxisDepth( const snap_result_t &r, const float *ref,
                         const float *axis, float *outDist )
{
    if ( !ref || !axis || !outDist )
        return false;
    if ( !r.valid || !KiwiSnap_IsGeometry( r.type ) )
        return false;

    float rel[3];
    Sub3( r.position, ref, rel );

    if ( r.type == SNAP_FACE )
    {
        // Snap results can survive a frame; validate the cached brush before reading
        // its face plane.
        const sel_item_t &it = r.source;
        if ( it.brush && Sel_BrushLive( it.brush ) && it.brush->def
          && it.brush->def->faces
          && it.faceIndex >= 0 && it.faceIndex < it.brush->def->faceCount )
        {
            const float *n = it.brush->def->faces[it.faceIndex].plane.normal;
            const float  nl = sqrtf( Dot3( n, n ) );
            if ( nl > 1.0e-6f )
            {
                const float un[3] = { n[0] / nl, n[1] / nl, n[2] / nl };
                const float den   = Dot3( un, axis );
                if ( fabsf( den ) < KSNAP_AXIS_PARALLEL )
                    return false;             // edge-on to the push: no depth to give
                *outDist = Dot3( un, rel ) / den;
                return true;
            }
        }
    // Without a usable face plane, preserve the prior point-projection behavior.
    }

    *outDist = Dot3( rel, axis );
    return true;
}
float KiwiSnap_LatticeAxis( float d, const float *ref, const float *axis,
                            bool hard, bool *outMajor )
{
    if ( outMajor )
        *outMajor = false;
    // The master grid switch disables both hard quantization and the soft band.
    if ( !KiwiGrid_SnapEnabled() )
        return d;
    const float g = KiwiUnits_GridSpacingWorld();
    if ( !( g > 0.0f ) || !ref || !axis )
        return d;
    // World axes snap the absolute coordinate; slanted axes snap signed distance.
    // scale converts a lattice correction back into gesture distance.
    float value = d;
    float scale = 1.0f;                 // d units per `value` unit
    int   worldAxis = -1;
    for ( int k = 0; k < 3; ++k )
        if ( fabsf( axis[k] ) > 0.999f )
            worldAxis = k;
    if ( worldAxis >= 0 )
    {
        value = ref[worldAxis] + axis[worldAxis] * d;
        scale = 1.0f / axis[worldAxis];               // |axis[k]| ~ 1, so +-1
    }

    const float cell    = floorf( value / g + 0.5f );
    const float snapped = cell * g;
    // Soft capture width is SCREEN PIXELS converted at the current point and capped
    // to a fraction of one WORLD-UNIT grid cell.
    float at[3];
    for ( int k = 0; k < 3; ++k )
        at[k] = ref[k] + axis[k] * d;
    float band = KSNAP_LIGHT_BAND_PIX * KiwiCam_WorldPerPixel( at );
    const float maxBand = g * KSNAP_LIGHT_MAX_FRAC;
    if ( !( band > 0.0f ) )  band = maxBand;          // NaN / degenerate camera
    if ( band > maxBand )    band = maxBand;
    // HARD uses a half-cell band only to size major preference. Its 1.5 multiplier
    // reaches 0.75 cell, so adjacent minor cells remain reachable.
    if ( hard )
        band = g * 0.5f;
    // Compute the nearest major independently; it wins inside its widened band.
    const float gMajor  = g * (float)KSNAP_MAJOR_STRIDE;
    const float majorAt = floorf( value / gMajor + 0.5f ) * gMajor;
    const float majBand = band * KSNAP_MAJOR_BAND_MUL;
    if ( fabsf( value - majorAt ) <= majBand )
    {
        if ( outMajor )
            *outMajor = true;
        return d + ( majorAt - value ) * scale;
    }

    if ( !hard && fabsf( value - snapped ) > band )
        return d;                                     // outside the band: untouched
    return d + ( snapped - value ) * scale;
}
// Area targets capture only inside their screen-scaled band.
float KiwiSnap_AreaMagnet( float cur, float target, const float *at )
{
    if ( !at )
        return cur;
    const float band = KSNAP_AREA_BAND_PIX * KiwiCam_WorldPerPixel( at );
    if ( !( band > 0.0f ) )                 // NaN / degenerate camera: keep the cursor
        return cur;
    return ( fabsf( target - cur ) <= band ) ? target : cur;
}
// Ranked snap query; see kiwi_snap.h for the exact winner order.
bool KiwiSnap_Query( const ray_t &ray, int imgX, int imgY, snap_result_t *out,
                     unsigned pickFlags )
{
    if ( !out )
        return false;
    *out = snap_result_t();
    s_labelX      = imgX;
    s_labelY      = imgY;
    s_haveEdgeDir = false;
    s_angleDeg    = 0.0f;
    s_angleIsRel  = false;
    s_angleRel    = 0.0f;
    s_axisHave    = false;
    // Recover the cursor pixel by round-tripping the normalized pick ray.
    const float ahead[3] = { ray.origin[0] + ray.dir[0],
                             ray.origin[1] + ray.dir[1],
                             ray.origin[2] + ray.dir[2] };
    float curX = 0.0f, curY = 0.0f;
    const bool haveCursorPx = Pick_WorldToImage( ahead, &curX, &curY );
    // Only shapes that cannot exist off-plane (rect/circle/arc/n-gon and planar
    // primitives) own a construction plane. Free line/polyline/spline tools do not.
    const bool planarPlacer = KiwiCon_PlanePlacement();
    const kconPlane_t &cplane = KiwiCon_ActivePlane();
    float cplaneHit[3] = { 0.0f, 0.0f, 0.0f };
    // Bounded intersection prevents grazing rays from resolving far beyond the
    // finite plane while retaining a usable in-plane answer.
    const bool haveCPlane = planarPlacer && KiwiCon_RayPlaneBounded( cplane, ray, cplaneHit );
    // Surface hit used by the raw ladder, occlusion gate, and area arm.
    const pick_result_t surf = Pick( ray, SEL_MASK_OBJECT, pickFlags );
    float raw[3];
    // Raw ladder: planar plane, surface, world ground, view-aligned fallback.
    // rawRung records which plane normal the frame must report.
    int rawRung = 3;
    if ( haveCPlane )
    {
        // A planar shape remains on its own plane even when snapping is disengaged.
        raw[0] = cplaneHit[0];
        raw[1] = cplaneHit[1];
        raw[2] = cplaneHit[2];
        rawRung = 0;
    }
    else if ( surf.valid )
    {
        raw[0] = surf.point[0];
        raw[1] = surf.point[1];
        raw[2] = surf.point[2];
        rawRung = 1;
    }
    else if ( RayHitsGroundPlane( ray, raw ) )
    {
        rawRung = 2;
    }
    else
    {
        // With no surface or ground crossing, use a view-facing plane through the
        // current tool anchor. This is essential in side orthographic views, whose pick
        // rays begin far behind the eye plane. Without an anchor, use the 512 WORLD-UNIT
        // fallback distance.
        RayPoint( ray, KSNAP_FALLBACK_DIST, raw );
        float viewN[3], anchor[3];
        if ( ViewPlaneNormal( ray, viewN ) && KiwiCon_ToolAnchor( anchor ) )
        {
            const float den = Dot3( ray.dir, viewN );
            if ( fabsf( den ) > 1.0e-4f )
            {
                float rel[3];
                Sub3( anchor, ray.origin, rel );
                const float t = Dot3( rel, viewN ) / den;
                if ( t > 0.0f && t < 1.0e6f )
                    RayPoint( ray, t, raw );
            }
        }
    }
    // Report the raw rung's plane before any ranked arm returns. Geometry targets
    // inherit it so bearings and placement share one coordinate space.
    out->havePlane = true;
    if ( rawRung == 0 )
        Copy3( cplane.normal, out->planeNormal );
    else if ( rawRung == 1 )
    {
        if ( surf.haveNormal )
            Copy3( surf.normal, out->planeNormal );
        else
            out->havePlane = false;          // a hit with no usable normal
    }
    else if ( rawRung == 2 )
    {
        out->planeNormal[0] = 0.0f;          // world ground
        out->planeNormal[1] = 0.0f;
        out->planeNormal[2] = 1.0f;
    }
    else if ( !ViewPlaneNormal( ray, out->planeNormal ) )
    {
        out->havePlane = false;              // the view-aligned rung, same normal
    }                                        // the raw point above was seated on
    // Engagement is context-wide: construction snaps by default and Ctrl frees it;
    // transforms are free by default and Ctrl engages it; pivot placement is always
    // engaged. SNAP_NONE remains valid and carries the raw position.
    if ( !KiwiCmd_SnapEngaged() )
    {
        out->valid = true;
        out->type  = SNAP_NONE;
        out->position[0] = raw[0];
        out->position[1] = raw[1];
        out->position[2] = raw[2];
        return true;
    }
    // Occlusion reference distance along the unit ray. Candidates behind it get
    // KSNAP_OCCLUDE_SLOP_PX SCREEN PIXELS of depth slack at their own scale.
    const bool haveSurfDepth = surf.valid;
    float      surfT         = 0.0f;
    if ( haveSurfDepth )
    {
        const float rel[3] = { surf.point[0] - ray.origin[0],
                               surf.point[1] - ray.origin[1],
                               surf.point[2] - ray.origin[2] };
        surfT = Dot3( rel, ray.dir );
    }
    // A planar placer may intentionally work behind the visible surface, so use
    // the farther of surface depth and its own plane depth.
    if ( haveSurfDepth && haveCPlane )
    {
        const float rel[3] = { cplaneHit[0] - ray.origin[0],
                               cplaneHit[1] - ray.origin[1],
                               cplaneHit[2] - ray.origin[2] };
        const float t = Dot3( rel, ray.dir );
        if ( t > surfT )
            surfT = t;
    }
    // This occlusion test is the only candidate depth gate.
    struct CandGate
    {
        bool         on;                 // surface occlusion enabled
        float        surfT;
        const ray_t *ray;
        bool operator()( const float *p ) const
        {
            if ( !p )
                return true;
            if ( on )
            {
                const float rel[3] = { p[0] - ray->origin[0],
                                       p[1] - ray->origin[1],
                                       p[2] - ray->origin[2] };
                const float t = Dot3( rel, ray->dir );
                if ( t > surfT )
                {
                    const float slop = KSNAP_OCCLUDE_SLOP_PX * KiwiCam_WorldPerPixel( p );
                    if ( ( t - surfT ) > slop )
                        return false;    // behind the surface under the cursor
                }
            }
            return true;
        }
    };
    CandGate visible;
    visible.on      = haveSurfDepth;
    visible.surfT   = surfT;
    visible.ray     = &ray;
    // Gather each construction candidate class once.
    conBest_t conAnchor, conSeg, conIsect, conMid;
    if ( haveCursorPx && KiwiCon_Count() > 0 )
    {
        ScanConAnchors      ( curX, curY, &conAnchor );
        ScanConIntersections( curX, curY, &conIsect );
        ScanConMidpoints    ( curX, curY, &conMid );
        ScanConSegments     ( curX, curY, &conSeg );
    }
    // Top priority: an open chain's first point within 8 SCREEN PIXELS.
    // The chain is not in the store until Finish(); exact closure must beat scene points
    // or a visually closed loop can retain a tiny non-closing gap.
    if ( haveCursorPx )
    {
        float loopStart[3];
        if ( KiwiCon_ToolLoopStart( loopStart ) )
        {
            float sx, sy;
            if ( Pick_WorldToImage( loopStart, &sx, &sy ) )
            {
                const float dx = sx - curX, dy = sy - curY;
                if ( sqrtf( dx * dx + dy * dy ) <= PICK_VERT_PIXELS )
                {
                    out->valid = true;
                    out->type  = SNAP_ENDPOINT;
                    out->position[0] = loopStart[0];
                    out->position[1] = loopStart[1];
                    out->position[2] = loopStart[2];
                    return true;
                }
            }
        }
    }
    // Point rank 1: construction anchor, 7 px and not occluded.
    if ( conAnchor.hit && visible( conAnchor.pos ) )
    {
        out->valid = true;
        out->type  = SNAP_ENDPOINT;
        out->position[0] = conAnchor.pos[0];
        out->position[1] = conAnchor.pos[1];
        out->position[2] = conAnchor.pos[2];
        return true;                     // `source` stays the null item — see the header
    }
    // Point rank 2: brush corner or patch control point, 8 px and not occluded.
    const pick_result_t vert = Pick( ray, SEL_MASK_VERTEX, pickFlags );
    if ( vert.valid && visible( vert.point ) )
    {
        out->valid  = true;
        out->type   = SNAP_VERTEX;
        out->source = vert.item;
        out->position[0] = vert.point[0];
        out->position[1] = vert.point[1];
        out->position[2] = vert.point[2];
        return true;
    }
    // Point rank 3: construction crossing, 7 px and not occluded.
    if ( conIsect.hit && visible( conIsect.pos ) )
    {
        out->valid = true;
        out->type  = SNAP_INTERSECTION;
        out->position[0] = conIsect.pos[0];
        out->position[1] = conIsect.pos[1];
        out->position[2] = conIsect.pos[2];
        return true;
    }
    // Point rank 3b: construction midpoint, 7 px and not occluded.
    if ( conMid.hit && visible( conMid.pos ) )
    {
        out->valid = true;
        out->type  = SNAP_EDGE_MID;
        out->position[0] = conMid.pos[0];
        out->position[1] = conMid.pos[1];
        out->position[2] = conMid.pos[2];
        s_haveEdgeDir = conMid.haveDir;
        if ( conMid.haveDir )
            for ( int k = 0; k < 3; ++k )
                s_edgeDir[k] = conMid.dir[k];
        return true;                     // `source` stays null — see the header
    }
    // One brush-edge Pick supplies both its point-rank midpoint and line-rank point.
    const pick_result_t edge = Pick( ray, SEL_MASK_EDGE, pickFlags );
    if ( edge.valid )
    {
        float ea[3], eb[3];
        const bool haveEnds = Sel_EdgeEnds( edge.item, ea, eb );
        if ( haveEnds )
        {
            for ( int k = 0; k < 3; ++k )
                s_edgeDir[k] = eb[k] - ea[k];
            const float len = sqrtf( Dot3( s_edgeDir, s_edgeDir ) );
            if ( len > 1e-4f )
            {
                for ( int k = 0; k < 3; ++k )
                    s_edgeDir[k] /= len;
                s_haveEdgeDir = true;
            }
            // Point rank 4: brush midpoint within 7 SCREEN PIXELS.
            const float mid[3] = { ( ea[0] + eb[0] ) * 0.5f,
                                   ( ea[1] + eb[1] ) * 0.5f,
                                   ( ea[2] + eb[2] ) * 0.5f };
            float mx, my;
            if ( haveCursorPx && Pick_WorldToImage( mid, &mx, &my )
              && PixelDist( mx, my, curX, curY ) <= KSNAP_R_EDGE_MID
              && visible( mid ) )
            {
                out->valid  = true;
                out->type   = SNAP_EDGE_MID;
                out->source = edge.item;
                out->position[0] = mid[0];
                out->position[1] = mid[1];
                out->position[2] = mid[2];
                return true;
            }
        }
    }
    // Last point rank: brush-face vertex-average center within 6 SCREEN PIXELS.
    // It remains available to drawing tools; only the later area hit is planar-suppressed.
    if ( haveCursorPx )
    {
        const pick_result_t faceHit = Pick( ray, SEL_MASK_FACE, pickFlags );
        float centre[3];
        float cx, cy;
        if ( faceHit.valid && FaceCentroid( faceHit.item, centre )
          && Pick_WorldToImage( centre, &cx, &cy )
          && PixelDist( cx, cy, curX, curY ) <= KSNAP_R_FACE_CENTER
          && visible( centre ) )
        {
            out->valid  = true;
            out->type   = SNAP_FACE_CENTER;
            out->source = faceHit.item;
            out->position[0] = centre[0];
            out->position[1] = centre[1];
            out->position[2] = centre[2];
            return true;
        }
    }
    // Axis lines share the line rank. Planar placers use plane U/V/N plus world Z;
    // free tools use world X/Y/Z. An anchor is required in either case.
    axisBest_t axisBest;
    if ( haveCursorPx )
    {
        float anchor[3];
        if ( KiwiCon_ToolAnchor( anchor ) )
        {
            kconPlane_t axisBasis;
            if ( planarPlacer )
            {
                axisBasis = cplane;
            }
            else
            {
                const float o[3] = { 0.0f, 0.0f, 0.0f };
                const float n[3] = { 0.0f, 0.0f, 1.0f };
                const float u[3] = { 1.0f, 0.0f, 0.0f };
                // Free-tool basis is world X/Y/Z; the duplicate Z normal is removed.
                if ( !KiwiCon_MakePlane( o, n, u, &axisBasis ) )
                    axisBasis = cplane;          // degenerate: fall back, never crash
            }
            // Free tools also offer incident construction/brush edge directions through the
            // anchor. They are full-3D line candidates; planar shapes cannot use off-plane axes.
            float extDirs[KSNAP_MAX_EXT_AXES * 3] = { 0.0f };
            int   extCount = 0;
            if ( !planarPlacer )
            {
                // Rank incident directions by cursor collinearity before applying the cap.
                float aim[3];
                Sub3( raw, anchor, aim );
                const bool haveAim = Norm3( aim );
                anchorDir_t inc[KSNAP_MAX_EXT_AXES];
                extCount = GatherAnchorDirs( anchor, haveAim ? aim : nullptr,
                                             inc, KSNAP_MAX_EXT_AXES );
                for ( int i = 0; i < extCount; ++i )
                    Copy3( inc[i].dir, extDirs + (size_t)i * 3 );
            }

            ScanAxes( Ed_Camera(), anchor, axisBasis, extDirs, extCount,
                      curX, curY, &axisBest );
        }
        if ( axisBest.hit )
        {
            s_axisHave    = true;
            s_axisHalfLen = axisBest.halfLen;
            s_axisName    = axisBest.name;
            for ( int k = 0; k < 3; ++k )
            {
                s_axisAnchor[k] = anchor[k];
                s_axisDir[k]    = axisBest.dir[k];
            }
        }
    }
    // Line rank: visible brush edge, visible construction segment, and axis guide
    // compete by SCREEN-PIXEL distance. Axis guides are exempt from surface occlusion.
    const bool edgeVis = edge.valid && visible( edge.point );
    const bool conSegVis = conSeg.hit && visible( conSeg.pos );
    if ( edgeVis || conSegVis || axisBest.hit )
    {
        const bool useCon = conSegVis && ( !edgeVis || conSeg.dist < edge.screenDist );
        const float lineDist = useCon ? conSeg.dist
                                      : ( edgeVis ? edge.screenDist : 1.0e9f );
        if ( axisBest.hit && axisBest.dist <= lineDist )
        {
            out->valid = true;
            out->type  = SNAP_AXIS;
            out->position[0] = axisBest.pos[0];
            out->position[1] = axisBest.pos[1];
            out->position[2] = axisBest.pos[2];
            // SNAP_AXIS uses the same along-line marker tick as SNAP_EDGE.
            s_haveEdgeDir = true;
            for ( int k = 0; k < 3; ++k )
                s_edgeDir[k] = axisBest.dir[k];
            return true;                     // `source` stays the null item
        }
        // Keep a losing nearby axis guide visible; it still communicates the available line.
        out->valid = true;
        out->type  = SNAP_EDGE;
        if ( useCon )
        {
            out->position[0] = conSeg.pos[0];
            out->position[1] = conSeg.pos[1];
            out->position[2] = conSeg.pos[2];
            s_haveEdgeDir = conSeg.haveDir;
            if ( conSeg.haveDir )
                for ( int k = 0; k < 3; ++k )
                    s_edgeDir[k] = conSeg.dir[k];
        }
        else
        {
            out->source = edge.item;
            out->position[0] = edge.point[0];
            out->position[1] = edge.point[1];
            out->position[2] = edge.point[2];
        }
        return true;
    }
    // Free-tool SNAP_ANGLE runs after named point/line targets and before area/grid.
    // Measure in the canonical basis of this frame's resolved plane, matching the HUD.
    // It is a capture band: 6 SCREEN PIXELS of lateral miss, floored at 1 degree and
    // capped at 0.22 of one 15-degree step. Outside the band the point is untouched.
    // Rotate only the in-plane component, preserve out-of-plane offset, and quantize
    // length with the same absolute-world-axis rule as SNAP_AXIS.
    if ( !planarPlacer && out->havePlane && haveCursorPx )
    {
        float anchor[3];
        if ( KiwiCon_ToolAnchor( anchor ) )
        {
            // Canonical surface bearing basis keeps the lock and displayed angle identical.
            float cu[3], cv[3];
            KiwiCon_BearingBasis( out->planeNormal, cu, cv );

            float d[3];
            Sub3( raw, anchor, d );
            const float outOfPlane = Dot3( d, out->planeNormal );
            const float du  = Dot3( d, cu );
            const float dv  = Dot3( d, cv );
            const float len = sqrtf( du * du + dv * dv );
            if ( len > 1.0e-3f )
            {
                const float rawDeg  = atan2f( dv, du ) * 57.29577951f;
                const float stopDeg = floorf( rawDeg / KCON_ANGLE_STEP + 0.5f )
                                    * KCON_ANGLE_STEP;
                float miss = rawDeg - stopDeg;
                if ( miss < 0.0f )
                    miss = -miss;
                // Convert the lateral SCREEN-PIXEL band to degrees at this length, then apply
                // the minimum-degree floor and maximum-step-fraction cap.
                float band = KSNAP_ANGLE_MAX_FRAC * KCON_ANGLE_STEP;
                {
                    const float wpp = WorldPerPixel( Ed_Camera(), raw );
                    if ( wpp > 0.0f )
                    {
                        float deg = ( ( KSNAP_ANGLE_BAND_PIX * wpp ) / len )
                                  * 57.29577951f;
                        if ( deg < KSNAP_ANGLE_BAND_MIN )
                            deg = KSNAP_ANGLE_BAND_MIN;
                        if ( deg < band )
                            band = deg;
                    }
                }

                if ( miss <= band )
                {
                    const float rad = stopDeg * 0.01745329252f;
                    float dirW[3];
                    for ( int k = 0; k < 3; ++k )
                        dirW[k] = cu[k] * cosf( rad ) + cv[k] * sinf( rad );
                    float use = SnapAlongAxis( anchor, dirW, len );
                    if ( !( use > 0.0f ) )
                        use = len;           // the lattice reached past the anchor
                    for ( int k = 0; k < 3; ++k )
                        out->position[k] = anchor[k] + dirW[k] * use
                                         + out->planeNormal[k] * outOfPlane;
                    // Report the same wrapped canonical bearing shown by the HUD.
                    s_angleDeg   = KiwiCon_WrapDeg( stopDeg );
                    s_angleIsRel = false;
                    s_angleRel   = 0.0f;
                    out->valid = true;
                    out->type  = SNAP_ANGLE;
                    return true;
                }
            }
        }
    }
    // Area rank: exact unsnapped surface hit for free tools. Planar shapes suppress
    // this arm because projecting another surface's hit onto their plane misplaces the cursor.
    if ( surf.valid && !planarPlacer )
    {
        out->valid  = true;
        out->type   = SNAP_FACE;
        out->source = surf.item;
        out->position[0] = raw[0];
        out->position[1] = raw[1];
        out->position[2] = raw[2];
        return true;
    }
    // Planar placers only: hard 15-degree direction quantization, then their
    // world-anchored in-plane grid. Free tools have neither construction-plane arm.
    if ( haveCPlane )
    {
        float uv[2];
        KiwiCon_WorldToPlane( cplane, cplaneHit, uv );

        float anchor[3];
        if ( KiwiCon_ToolAnchor( anchor ) )
        {
            float auv[2];
            KiwiCon_WorldToPlane( cplane, anchor, auv );
            const float du = uv[0] - auv[0], dv = uv[1] - auv[1];
            const float len = sqrtf( du * du + dv * dv );
            if ( len > 1.0e-3f )
            {
                // Hard planar angle snap keeps cursor distance. If the anchor lies within
                // KREG_JOIN_DIST WORLD UNITS of a construction or brush edge, measure stops relative
                // to that line; otherwise use the canonical surface bearing basis. KCON_ANGLE_STEP
                // remains 15 degrees. phi0 maps canonical zero into the plane's authoring basis
                // without changing the plane lattice or shape coordinates.
                float phi0 = 0.0f;
                {
                    float cu[3], cv[3];
                    KiwiCon_BearingBasis( cplane.normal, cu, cv );
                    phi0 = atan2f( Dot3( cu, cplane.v ), Dot3( cu, cplane.u ) ) * 57.29577951f;
                }

                float baseDeg = 0.0f;
                {
                    float refDir[3];
                    if ( AnchorLineDir( anchor, refDir ) )
                    {
                        // Project the reference direction into the plane; ignore a perpendicular line.
                        const float ru = Dot3( refDir, cplane.u );
                        const float rv = Dot3( refDir, cplane.v );
                        if ( sqrtf( ru * ru + rv * rv ) > 1.0e-3f )
                        {
                            baseDeg      = atan2f( rv, ru ) * 57.29577951f - phi0;
                            s_angleIsRel = true;
                        }
                    }
                }

                const float rawDeg = atan2f( dv, du ) * 57.29577951f - phi0;
                float rel = rawDeg - baseDeg;
                rel = floorf( rel / KCON_ANGLE_STEP + 0.5f ) * KCON_ANGLE_STEP;
                const float deg = baseDeg + rel;                 // canonical degrees
                const float rad = ( deg + phi0 ) * 0.01745329252f;
                uv[0] = auv[0] + cosf( rad ) * len;
                uv[1] = auv[1] + sinf( rad ) * len;
                // Absolute label uses wrapped canonical degrees.
                s_angleDeg = KiwiCon_WrapDeg( deg );
                // Relative label remains in (-180, 180].
                while ( rel >  180.0f ) rel -= 360.0f;
                while ( rel <= -180.0f ) rel += 360.0f;
                s_angleRel = rel;
                KiwiCon_PlaneToWorld( cplane, uv, out->position );
                out->valid = true;
                out->type  = SNAP_ANGLE;
                return true;
            }
        }
        // Planar grid uses KiwiCon_SnapUV so its lattice remains world-anchored.
        KiwiCon_SnapUV( cplane, uv );
        KiwiCon_PlaneToWorld( cplane, uv, out->position );
        out->valid = true;
        out->type  = SNAP_CPLANE;
        return true;
    }
    // Last rank: grid-snap the raw ground/view-fallback point. If grid snapping is
    // disabled or spacing is unusable, return the raw point as valid SNAP_NONE.
    out->valid = true;
    out->type  = SNAP_GRID;
    if ( !KiwiGrid_Snap( raw, out->position ) )
    {
        out->type = SNAP_NONE;
    }
    return true;
}
// Shared preview/placement front door onto KiwiSnap_Query.
bool KiwiSnap_ResolvePoint( const ray_t &ray, int imgX, int imgY, float out[3] )
{
    if ( !out )
        return false;
    snap_result_t r;
    if ( !KiwiSnap_Query( ray, imgX, imgY, &r ) || !r.valid )
        return false;
    out[0] = r.position[0];
    out[1] = r.position[1];
    out[2] = r.position[2];
    return true;
}
// World-space marker emitted into the caller-owned line batch.
void KiwiSnap_EmitMarker( const snap_result_t &r )
{
    if ( !r.valid || !KiwiSnap_ShowMarkers() )
        return;

    camera_s *c = Ed_Camera();
    if ( c->width < 1 || c->height < 1 )
        return;
    // World markers share one near-black color; the label carries type color.
    KiwiLines_Color( KSNAP_COL_MARK[0], KSNAP_COL_MARK[1], KSNAP_COL_MARK[2] );

    const float wpp = WorldPerPixel( c, r.position );
    const float h   = wpp * 6.0f;
    float a[3], b[3];
    // All point ranks use a dot and separated ring. Midpoints may add an along-edge tick.
    if ( r.type == SNAP_VERTEX || r.type == SNAP_EDGE_MID
      || r.type == SNAP_ENDPOINT || r.type == SNAP_INTERSECTION
      || r.type == SNAP_FACE_CENTER )
    {
        EmitDotAndRing( c, r.position, wpp );
        // The along-edge tick distinguishes a midpoint from an isolated point.
        if ( r.type == SNAP_EDGE_MID && s_haveEdgeDir )
        {
            for ( int k = 0; k < 3; ++k )
            {
                a[k] = r.position[k] - s_edgeDir[k] * ( h * 2.0f );
                b[k] = r.position[k] + s_edgeDir[k] * ( h * 2.0f );
            }
            AddNudged( c, a, b );
        }
        return;
    }
    // Angle snaps draw a cross plus a tick along the locked direction.
    if ( r.type == SNAP_ANGLE )
    {
        float anchor[3];
        if ( KiwiCon_ToolAnchor( anchor ) )
        {
            float dir[3];
            for ( int k = 0; k < 3; ++k )
                dir[k] = r.position[k] - anchor[k];
            const float l = sqrtf( Dot3( dir, dir ) );
            if ( l > 1e-4f )
            {
                for ( int k = 0; k < 3; ++k )
                {
                    dir[k] /= l;
                    a[k] = r.position[k] - dir[k] * ( h * 2.5f );
                    b[k] = r.position[k] + dir[k] * ( h * 2.5f );
                }
                AddNudged( c, a, b );
            }
        }
        for ( int k = 0; k < 3; ++k )
        {
            a[k] = r.position[k] - c->vup[k] * h;
            b[k] = r.position[k] + c->vup[k] * h;
        }
        AddNudged( c, a, b );
        return;
    }
    // Line ranks draw an along-line tick and a camera-facing cross-tick.
    if ( r.type == SNAP_EDGE || r.type == SNAP_AXIS )
    {
        if ( s_haveEdgeDir )
        {
            for ( int k = 0; k < 3; ++k )
            {
                a[k] = r.position[k] - s_edgeDir[k] * ( h * 2.0f );
                b[k] = r.position[k] + s_edgeDir[k] * ( h * 2.0f );
            }
            AddNudged( c, a, b );
        }
        for ( int k = 0; k < 3; ++k )
        {
            a[k] = r.position[k] - c->vup[k] * ( h * 0.6f );
            b[k] = r.position[k] + c->vup[k] * ( h * 0.6f );
        }
        AddNudged( c, a, b );
        return;
    }
    // Area rank draws a small diamond and traces the snapped face winding so the
    // chosen plane is visible. The winding is bounded by one face and the line budget.
    if ( r.type == SNAP_FACE )
    {
        if ( r.source.brush && Sel_BrushLive( r.source.brush )
          && r.source.brush->def && r.source.faceIndex >= 0
          && r.source.faceIndex < r.source.brush->def->faceCount
          && r.source.brush->def->faces )
        {
            const winding_t *w = r.source.brush->def->faces[r.source.faceIndex].w;
            if ( w && w->numpoints >= 3 )
                for ( int i = 0, j = w->numpoints - 1; i < w->numpoints; j = i++ )
                    AddNudged( c, w->p[j], w->p[i] );
        }
        const float d = h * 0.45f;
        float p[4][3];
        const float sx[4] = { -1.0f, 0.0f, 1.0f, 0.0f };
        const float sy[4] = {  0.0f, 1.0f, 0.0f, -1.0f };
        for ( int i = 0; i < 4; ++i )
            for ( int k = 0; k < 3; ++k )
                p[i][k] = r.position[k] + c->vright[k] * ( sx[i] * d )
                                        + c->vup[k]    * ( sy[i] * d );
        for ( int i = 0; i < 4; ++i )
            AddNudged( c, p[i], p[( i + 1 ) & 3] );
        return;
    }

    // Grid / no-snap: a camera-facing cross, so it reads against grid lines.
    for ( int k = 0; k < 3; ++k )
    {
        a[k] = r.position[k] - c->vright[k] * h;
        b[k] = r.position[k] + c->vright[k] * h;
    }
    AddNudged( c, a, b );
    for ( int k = 0; k < 3; ++k )
    {
        a[k] = r.position[k] - c->vup[k] * h;
        b[k] = r.position[k] + c->vup[k] * h;
    }
    AddNudged( c, a, b );
}
// Advertise the hovered face's actual point targets: corners, edge midpoints,
// and vertex-average center. Requires a compatible live command, visible markers,
// and engaged snapping. Own batch is capped at KSNAP_ACCENT_MAX points.
void KiwiSnap_DrawFaceAccents()
{
    if ( !KiwiSnap_ShowMarkers() )
        return;
    // Pivot placement and held transform handles may request the same dots.
    // A bounding-box center is omitted because no snap arm can honor it.
    KiwiEditorCommand *cmd = KiwiCmd_Active();
    if ( !cmd || ( !cmd->WantsClicks() && !KiwiXform_WantsSnapDots() ) )
        return;
    // Do not advertise targets while snapping is disengaged.
    if ( !KiwiCmd_SnapEngaged() )
        return;

    camera_s *c = Ed_Camera();
    if ( c->width < 1 || c->height < 1 )
        return;

    int   x = 0, y = 0;
    ray_t ray;
    if ( !KiwiCmd_LastCursor( &x, &y ) || !Pick_RayFromImagePos( x, y, &ray ) )
        return;
    // SEL_MASK_FACE without OBJECT is load-bearing: Pick resolves faceIndex only
    // in face-granularity mode. Selection mode does not change placement targets.
    const pick_result_t hit = Pick( ray, SEL_MASK_FACE );
    if ( !hit.valid || hit.item.kind != SEL_FACE || !hit.item.brush )
        return;
    brush_t *def = hit.item.brush->def;
    if ( !def || !def->faces || hit.item.faceIndex < 0
      || hit.item.faceIndex >= def->faceCount )
        return;
    const winding_t *w = def->faces[hit.item.faceIndex].w;
    if ( !w || w->numpoints < 3 || w->numpoints > MAX_POINTS_ON_WINDING )
        return;

    const int perDot = KSNAP_ACCENT_SEGS + KSNAP_ACCENT_SEGS / 2;
    KiwiLines_Begin( KSNAP_ACCENT_MAX * perDot, 1 );
    // Use the same near-black ink as the committed marker.
    KiwiLines_Color( KSNAP_COL_MARK[0], KSNAP_COL_MARK[1], KSNAP_COL_MARK[2] );

    int   emitted = 0;
    float centre[3] = { 0.0f, 0.0f, 0.0f };

    for ( int i = 0; i < w->numpoints; ++i )
    {
        for ( int k = 0; k < 3; ++k )
            centre[k] += w->p[i][k];
        // Emit each corner and its outgoing edge midpoint in one bounded pass.
        if ( emitted < KSNAP_ACCENT_MAX )
        {
            float p[3];
            Nudge( c, w->p[i], p );
            EmitDisc( c, p, KSNAP_ACCENT_PIX * WorldPerPixel( c, w->p[i] ),
                      KSNAP_ACCENT_SEGS, true );
            ++emitted;
        }
        if ( emitted < KSNAP_ACCENT_MAX )
        {
            const int j = ( i + 1 ) % w->numpoints;
            float mid[3];
            for ( int k = 0; k < 3; ++k )
                mid[k] = ( w->p[i][k] + w->p[j][k] ) * 0.5f;
            float p[3];
            Nudge( c, mid, p );
            EmitDisc( c, p, KSNAP_ACCENT_PIX * WorldPerPixel( c, mid ),
                      KSNAP_ACCENT_SEGS, true );
            ++emitted;
        }
    }
    // Emit the derived center last so corners and midpoints own a tight budget.
    if ( emitted < KSNAP_ACCENT_MAX )
    {
        const float inv = 1.0f / (float)w->numpoints;
        for ( int k = 0; k < 3; ++k )
            centre[k] *= inv;
        float p[3];
        Nudge( c, centre, p );
        EmitDisc( c, p, KSNAP_ACCENT_PIX * WorldPerPixel( c, centre ),
                  KSNAP_ACCENT_SEGS, true );
    }

    KiwiLines_Flush();
}
// Emit one tool-owned spot into the current line batch. pixRadius is SCREEN
// PIXELS at p's depth; the caller owns color and capacity.
void KiwiSnap_EmitSpot( const float *p, float pixRadius, bool solid )
{
    if ( !p || !( pixRadius > 0.0f ) )
        return;
    camera_s *c = Ed_Camera();
    if ( c->width < 1 || c->height < 1 )
        return;
    float n[3];
    Nudge( c, p, n );
    EmitDisc( c, n, pixRadius * WorldPerPixel( c, p ),
              solid ? KSNAP_ACCENT_SEGS : KSNAP_RING_SEGS, solid );
}
// Publish glyph sizes so tool previews do not duplicate constants.
float KiwiSnap_AccentPixels() { return KSNAP_ACCENT_PIX; }
float KiwiSnap_RingPixels()   { return KSNAP_RING_PIX;   }
// Draw only a captured axis. Dashes are geometry because kiwi_lines has no
// dash mode or alpha. The guide owns a bounded 96-segment batch.
void KiwiSnap_DrawAxisGuides()
{
    if ( !s_axisHave || !KiwiSnap_ShowMarkers() )
        return;

    KiwiEditorCommand *cmd = KiwiCmd_Active();
    if ( !cmd || !cmd->WantsClicks() )
        return;
    // Axis guides follow the same engagement gate as their snap candidates.
    if ( !KiwiCmd_SnapEngaged() )
        return;

    camera_s *c = Ed_Camera();
    if ( c->width < 1 || c->height < 1 )
        return;

    const float wpp = WorldPerPixel( c, s_axisAnchor );
    if ( !( wpp > 0.0f ) || !( s_axisHalfLen > 0.0f ) )
        return;
    const float step = s_axisHalfLen / (float)KSNAP_AXIS_DASHES;
    const float dash = step * KSNAP_AXIS_DASH_DUTY;
    if ( !( step > 0.0f ) )
        return;

    KiwiLines_Begin( KSNAP_AXIS_MAX_SEGS, 1 );
    KiwiLines_Color( KSNAP_COL_AXIS[0], KSNAP_COL_AXIS[1], KSNAP_COL_AXIS[2] );
    // Mirror one dash pattern through the anchor.
    for ( int i = 0; i < KSNAP_AXIS_DASHES; ++i )
    {
        const float t  = (float)i * step;
        const float t2 = t + dash;
        float a[3], b[3], na[3], nb[3];
        for ( int k = 0; k < 3; ++k )
        {
            a[k] = s_axisAnchor[k] + s_axisDir[k] * t;
            b[k] = s_axisAnchor[k] + s_axisDir[k] * t2;
        }
        Nudge( c, a, na );
        Nudge( c, b, nb );
        if ( !KiwiLines_Add( na, nb ) )
            break;
        for ( int k = 0; k < 3; ++k )
        {
            a[k] = s_axisAnchor[k] - s_axisDir[k] * t;
            b[k] = s_axisAnchor[k] - s_axisDir[k] * t2;
        }
        Nudge( c, a, na );
        Nudge( c, b, nb );
        if ( !KiwiLines_Add( na, nb ) )
            break;
    }
    // Mark the source anchor so the guide origin is unambiguous.
    if ( KiwiLines_Remaining() >= KSNAP_ACCENT_SEGS + KSNAP_ACCENT_SEGS / 2 )
    {
        float p[3];
        Nudge( c, s_axisAnchor, p );
        EmitDisc( c, p, KSNAP_ACCENT_PIX * wpp, KSNAP_ACCENT_SEGS, true );
    }

    KiwiLines_Flush();
}
// Screen-space label drawn during the ImGui frame.
void KiwiSnap_DrawLabel( const snap_result_t &r, float imgMinX, float imgMinY,
                         float imgW, float imgH )
{
    if ( !r.valid || !KiwiSnap_ShowMarkers() )
        return;
    // Display every position through the shared inches formatter.
    char bx[32], by[32], bz[32];
    KiwiUnits_Format( bx, sizeof( bx ), r.position[0] );
    KiwiUnits_Format( by, sizeof( by ), r.position[1] );
    KiwiUnits_Format( bz, sizeof( bz ), r.position[2] );

    char text[160];
    if ( r.type == SNAP_ANGLE && s_angleIsRel )
        // Distinguish a relative stop from the same absolute bearing.
        _snprintf( text, sizeof( text ), "%.0f deg rel   %s, %s, %s",
                   (double)s_angleRel, bx, by, bz );
    else if ( r.type == SNAP_ANGLE )
        _snprintf( text, sizeof( text ), "%.0f deg   %s, %s, %s",
                   (double)s_angleDeg, bx, by, bz );
    else if ( r.type == SNAP_AXIS )
        // Include the axis name so a captured guide is explicit.
        _snprintf( text, sizeof( text ), "%s axis   %s, %s, %s",
                   s_axisName, bx, by, bz );
    // SNAP_NONE means snapping is not engaged. Spell out the context-specific Ctrl
    // action instead of presenting the ambiguous short tag "off".
    else if ( r.type == SNAP_NONE )
    {
        const KiwiEditorCommand *cmd = KiwiCmd_Active();
        const bool construct = cmd
            && cmd->SnapContext() == KiwiEditorCommand::KSNAPCTX_CONSTRUCT;
        _snprintf( text, sizeof( text ), "%s   %s, %s, %s",
                   construct ? "snap free (Ctrl)" : "snap off - hold Ctrl",
                   bx, by, bz );
    }
    else
        _snprintf( text, sizeof( text ), "%s   %s, %s, %s",
                   KiwiSnap_TypeName( r.type ), bx, by, bz );
    text[sizeof( text ) - 1] = '\0';

    const ImVec2 sz = ImGui::CalcTextSize( text );
    const ImVec2 pad( 5.0f, 3.0f );
    // Place below-right of the cursor and clamp inside the camera image.
    float x = imgMinX + (float)s_labelX + 16.0f;
    float y = imgMinY + (float)s_labelY + 14.0f;
    const float maxX = imgMinX + imgW - ( sz.x + pad.x * 2.0f ) - 2.0f;
    const float maxY = imgMinY + imgH - ( sz.y + pad.y * 2.0f ) - 2.0f;
    if ( x > maxX ) x = maxX;
    if ( y > maxY ) y = maxY;
    if ( x < imgMinX ) x = imgMinX;
    if ( y < imgMinY ) y = imgMinY;
    // Labels are the only per-type colors. Shared midpoint/edge types use their
    // point/line tint; construction-only types use the construction tint.
    ImU32 tint = IM_COL32( 220, 220, 235, 255 );                       // grid / off
    if ( r.type == SNAP_VERTEX || r.type == SNAP_EDGE_MID
      || r.type == SNAP_FACE_CENTER )
        tint = IM_COL32( 120, 255, 145, 255 );                         // point
    else if ( r.type == SNAP_EDGE || r.type == SNAP_AXIS )
        tint = IM_COL32( 255, 220, 100, 255 );                         // line
    else if ( r.type == SNAP_FACE )
        tint = IM_COL32( 150, 195, 255, 255 );                         // area
    else if ( r.type == SNAP_ENDPOINT || r.type == SNAP_INTERSECTION
           || r.type == SNAP_CPLANE   || r.type == SNAP_ANGLE )
        tint = IM_COL32( 255, 140, 205, 255 );                         // construction

    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled( ImVec2( x, y ),
                       ImVec2( x + sz.x + pad.x * 2.0f, y + sz.y + pad.y * 2.0f ),
                       IM_COL32( 18, 18, 22, 205 ), 3.0f );
    dl->AddText( ImVec2( x + pad.x, y + pad.y ), tint, text );
}

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
#include "kiwi_droptrace.h" // xmodel mesh vertices (rank 2)
#include "kiwi_conselect.h" // construction self-snap exclusion
#include "kiwi_construct.h"
#include "kiwi_transform.h" // pivot placement and snap-dot state
#include "kiwi_grid.h"
#include "kiwi_hover.h"       // KIWI: the cylinder overlay's hovered target
#include "kiwi_region.h"      // KREG_JOIN_DIST weld tolerance
#include "kiwi_section.h"     // KIWI: KiwiView_XRay, section clamp/visibility (line of sight)
#include "kiwi_lines.h"
#include "kiwi_ported.h"
#include "kiwi_primitive.h"
#include "kiwi_units.h"
#include "kiwi_ux.h"          // KIWI: KiwiUX_ShowHover (the cylinder overlay's hovered target)
#include "radiant_registry.h"
#include "kiwi_vec.h"         // Dot3/Sub3/Cross3/Norm3

#include <math.h>
#include <stdio.h>
#include <algorithm>
#include <functional>

namespace
{
    bool CylinderUsable( selbrush_t *b )
    {
        if ( !Pick_BrushPickable( b ) ) return false;
        return b->owner == world_entity ||
            ( b->owner->def && b->owner->def->eclass && !b->owner->def->eclass->fixedsize );
    }

    // Does the ray pass within padPx SCREEN PIXELS (at the box centre) of the brush bounds?
    bool BoundsNearRay( const brush_t *def, const ray_t &ray, float padPx )
    {
        float centre[3];
        for ( int k = 0; k < 3; ++k ) centre[k] = ( def->mins[k] + def->maxs[k] ) * 0.5f;
        const float pad = padPx * KiwiCam_WorldPerPixel( centre );
        float lo = 0.0f, hi = 1.0e30f;
        for ( int k = 0; k < 3; ++k )
        {
            const float a = def->mins[k] - pad, b = def->maxs[k] + pad;
            if ( fabsf( ray.dir[k] ) < 1.0e-8f )
            {
                if ( ray.origin[k] < a || ray.origin[k] > b ) return false;
                continue;
            }
            float t0 = ( a-ray.origin[k] ) / ray.dir[k], t1 = ( b-ray.origin[k] ) / ray.dir[k];
            if ( t0 > t1 ) { const float t = t0; t0 = t1; t1 = t; }
            if ( t0 > lo ) lo = t0;
            if ( t1 < hi ) hi = t1;
            if ( lo > hi ) return false;
        }
        return true;
    }

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
    // KIWI (2026-10-03): xmodel mesh vertices - candidates tried per snap query (nearest
    // first, until one passes the depth gate) and dots drawn near the cursor.
    const int   KSNAP_MODEL_VERT_TRY = 8;
    const int   KSNAP_MODEL_DOTS_MAX = 48;
    const int   KSNAP_DOT_SEGS  = 6;       // hexagon — reads as solid at 2 px
    const int   KSNAP_RING_SEGS = 12;      // 12 chords read as round at 6 px

    int   s_labelX = 0;
    int   s_labelY = 0;
    // KIWI (2026-10-22, Plasticity 2026.2 mesh snapping): the triangle of the xmodel vertex
    // the last query snapped to, drawn by KiwiSnap_DrawLabel while that snap stands.
    bool  s_meshTriValid = false;
    float s_meshTriAt[3] = { 0.0f, 0.0f, 0.0f };   // the snapped vertex it belongs to
    float s_meshTri[3][3];
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
    enum { KSNAP_MAX_EXT_AXES = 4 };   // 3 -> 4 (2026-09-21): the chain adds par/perp guides
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
            const float th = ( (float)( 2.0 * KIWI_PI ) * (float)( i % segs ) ) / (float)segs;
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
            const float th = ( (float)( 2.0 * KIWI_PI ) * (float)i ) / (float)segs;
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
        bool  quarter = false;                 // KIWI: a quarter point, not the midpoint
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
                t = Pick_ScreenToWorldT( wa, wb, t );     // KIWI (2026-10-26): perspective-correct
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
                              const char *const *extNames, int extCount, axisCand_t *out )
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
            // "par" / "perp" say which chain relation the guide holds (GatherAnchorDirs).
            out[n].name = AxisName( d, ( extNames && extNames[i] ) ? extNames[i] : "ext" );
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
                   const float *extDirs, const char *const *extNames, int extCount,
                   float curX, float curY, axisBest_t *best )
    {
        const float wpp = WorldPerPixel( c, anchor );
        if ( !( wpp > 0.0f ) )
            return;
        float half = KSNAP_AXIS_GUIDE_PIX * wpp;
        if ( half < 32.0f )    half = 32.0f;
        if ( half > 32768.0f ) half = 32768.0f;

        axisCand_t cand[KSNAP_AXIS_CANDS];
        const int n = GatherAxisCandidates( plane, extDirs, extNames, extCount, cand );

        for ( int i = 0; i < n; ++i )
        {
            // The guide half-length is shortened until BOTH ends project — an axis
            // through a point near the eye plane runs behind the camera at full
            // length, and Pick_WorldToImage refuses those (as it must).
            float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
            float use = half;
            bool  ok  = false;
            float wa[3], wb[3];
            for ( int tryI = 0; tryI < 3 && !ok; ++tryI )
            {
                for ( int k = 0; k < 3; ++k )
                {
                    wa[k] = anchor[k] - cand[i].dir[k] * use;
                    wb[k] = anchor[k] + cand[i].dir[k] * use;
                }
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
            // KIWI (2026-10-26, user: "it wont let me create a new construction line that is
            // going upwards"): the screen fraction is NOT the world fraction in perspective.
            // A Z guide climbing toward a camera above projects its upper half far longer
            // than its lower half, so the old linear map pinned the point near the anchor
            // however far up the cursor went.
            const float along = SnapAlongAxis( anchor, cand[i].dir,
                                               -use + ( 2.0f * use ) * Pick_ScreenToWorldT( wa, wb, t ) );

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
    // KIWI (2026-09-25, user: "the mid and quarter points should be small black nubs that I
    // can see to point onto and lock on to"): segment DIVISION points - the midpoint, and the
    // quarter points once the segment is KSNAP_QUARTER_MIN_PX long on screen (shorter, they
    // would crowd the corners).  Brush edges offer them from every edge near the cursor: the
    // edge Pick names ONE edge, often a neighbour's shorter collinear one, so the midpoint of
    // the edge actually aimed at could be unreachable.
    const float KSNAP_QUARTER_MIN_PX = 48.0f;
    const float KSNAP_DIV_REACH_PX   = 60.0f;   // edges this close to the cursor show their nubs
    const float KSNAP_MID_NUB_PX     = 2.4f;    // nub radii as drawn (user: 20% smaller than 3 / 2.25)
    const float KSNAP_QUARTER_NUB_PX = 1.8f;
    const float KSNAP_NUB_MIN_SEG_PX = 16.0f;   // shorter segments (curve tessellation) get no nub
    const int   KSNAP_DIV_DRAW_MAX   = 96;      // nubs drawn per frame

    struct divPt_t
    {
        float pos[3];
        float dir[3];                  // unit segment direction
        float px, py;                  // camera-image pixels
        float segPx;                   // the segment's projected length
        bool  quarter;
    };

    // A segment's LINE (KIWI 2026-09-25, user: a side "isn't being considered as a possible
    // long line instead of 2-3 chunks").  Modern Plasticity's snap cache (app_window.jsc:
    // "edges.halves", "edges.circles.quarters", "segments.halves", ...) takes midpoints per
    // TOPOLOGICAL edge, and the straight side of a solid - or of a union of solids - is one
    // edge there.  Here a straight side is several pieces: brush edges of separate convex
    // brushes, or construction segments split where other lines were drawn to them (the
    // powerplant building outline: one polyline with vertices at a connector's corners).  So
    // the aimed segment grows over every segment of its own kind lying along it that overlaps
    // or touches it, and that line's mid / quarter points replace the pieces'.  The points are
    // taken ON the pieces (ChainPoint), never on an idealised chord.
    const float KSNAP_LINE_PERP   = 0.5f;       // WORLD UNITS a piece's ends may lie off the line
    const float KSNAP_LINE_SLOPE  = 0.005f;     // ...plus this per unit beyond the aimed segment
    const float KSNAP_LINE_GAP    = 0.5f;       // WORLD UNITS between touching pieces
    const float KSNAP_LINE_COS    = 0.999f;     // |cos| between their directions (~2.6 deg)
    const int   KSNAP_LINE_PASSES = 8;          // growth passes; each one can only lengthen
    const int   KSNAP_LINE_PIECES = 64;         // pieces one line keeps

    struct linePiece_t
    {
        float a[3], b[3];
        float ta, tb;                           // a's and b's parameters along the line
    };

    struct edgeLine_t
    {
        float o[3], dir[3];                     // the aimed segment's start, unit direction
        float s0, s1;                           // the aimed segment's own extent
        float t0, t1;                           // the line's extent
        std::vector<linePiece_t> pieces;
    };

    // Is world segment p-q along L (parallel, both ends within tolerance of it)?  Its ends'
    // parameters land in *ta (p) and *tb (q).
    bool OnLine( const edgeLine_t &L, const float *p, const float *q, float *ta, float *tb )
    {
        float e[3];
        Sub3( q, p, e );
        if ( !Norm3( e, 1.0e-4f ) || fabsf( Dot3( e, L.dir ) ) < KSNAP_LINE_COS )
            return false;
        const float *ends[2] = { p, q };
        float t[2];
        for ( int n = 0; n < 2; ++n )
        {
            float r[3];
            Sub3( ends[n], L.o, r );
            t[n] = Dot3( r, L.dir );
            for ( int k = 0; k < 3; ++k )
                r[k] -= L.dir[k] * t[n];
            const float beyond = t[n] < L.s0 ? L.s0 - t[n] : ( t[n] > L.s1 ? t[n] - L.s1 : 0.0f );
            const float tol    = KSNAP_LINE_PERP + KSNAP_LINE_SLOPE * beyond;
            if ( Dot3( r, r ) > tol * tol )
                return false;
        }
        *ta = t[0];
        *tb = t[1];
        return true;
    }

    // L := the line of segment p-q alone; false when degenerate.
    bool SeedLine( edgeLine_t &L, const float *p, const float *q )
    {
        float e[3];
        Sub3( q, p, e );
        Copy3( e, L.dir );
        if ( !Norm3( L.dir, 1.0e-4f ) )
            return false;
        Copy3( p, L.o );
        L.s0 = L.t0 = 0.0f;
        L.s1 = L.t1 = Dot3( e, L.dir );
        L.pieces.clear();
        linePiece_t s;
        Copy3( p, s.a );
        Copy3( q, s.b );
        s.ta = L.t0;
        s.tb = L.t1;
        L.pieces.push_back( s );
        return true;
    }

    // Adds p-q (ends at ta / tb along L) when it touches L and is not in it yet (a brush edge
    // comes once per face); true when L got longer.
    bool AddPiece( edgeLine_t &L, const float *p, const float *q, float ta, float tb )
    {
        const float lo = ta < tb ? ta : tb, hi = ta < tb ? tb : ta;
        if ( hi < L.t0 - KSNAP_LINE_GAP || lo > L.t1 + KSNAP_LINE_GAP
          || (int)L.pieces.size() >= KSNAP_LINE_PIECES )
            return false;
        for ( const linePiece_t &s : L.pieces )
        {
            const float slo = s.ta < s.tb ? s.ta : s.tb, shi = s.ta < s.tb ? s.tb : s.ta;
            if ( fabsf( slo - lo ) < 1.0e-2f && fabsf( shi - hi ) < 1.0e-2f )
                return false;
        }
        linePiece_t s;
        Copy3( p, s.a );
        Copy3( q, s.b );
        s.ta = ta;
        s.tb = tb;
        L.pieces.push_back( s );
        bool grew = false;
        if ( lo < L.t0 - 1.0e-3f ) { L.t0 = lo; grew = true; }
        if ( hi > L.t1 + 1.0e-3f ) { L.t1 = hi; grew = true; }
        return grew;
    }

    // The point of L at parameter t, on the piece that spans it (the chord across a gap).
    void ChainPoint( const edgeLine_t &L, float t, float *out )
    {
        for ( const linePiece_t &s : L.pieces )
        {
            const float lo = s.ta < s.tb ? s.ta : s.tb, hi = s.ta < s.tb ? s.tb : s.ta;
            if ( t < lo - 1.0e-3f || t > hi + 1.0e-3f || hi - lo < 1.0e-4f )
                continue;
            const float u = ( t - s.ta ) / ( s.tb - s.ta );
            for ( int k = 0; k < 3; ++k )
                out[k] = s.a[k] + ( s.b[k] - s.a[k] ) * u;
            return;
        }
        for ( int k = 0; k < 3; ++k )
            out[k] = L.o[k] + L.dir[k] * t;
    }

    // Lengthens L over every segment that lies along it and touches it.  eachSegment( lo, hi,
    // visit ) calls visit( p, q ) for the segments of L's kind whose bounds meet box lo..hi.
    template <class E> void GrowLine( edgeLine_t &L, E eachSegment )
    {
        for ( int pass = 0; pass < KSNAP_LINE_PASSES; ++pass )
        {
            bool        grew = false;
            float       lo[3], hi[3];
            const float pad = KSNAP_LINE_GAP + KSNAP_LINE_PERP + KSNAP_LINE_SLOPE * ( L.t1 - L.t0 );
            for ( int k = 0; k < 3; ++k )
            {
                const float s = L.o[k] + L.dir[k] * L.t0, e = L.o[k] + L.dir[k] * L.t1;
                lo[k] = ( s < e ? s : e ) - pad;
                hi[k] = ( s < e ? e : s ) + pad;
            }
            eachSegment( lo, hi, [&]( const float *p, const float *q )
            {
                float ta, tb;
                if ( OnLine( L, p, q, &ta, &tb ) && AddPiece( L, p, q, ta, tb ) )
                    grew = true;
            } );
            if ( !grew )
                break;
        }
    }

    // fn( const divPt_t & ) for each division point of L - the midpoint, and the quarter
    // points once L is KSNAP_QUARTER_MIN_PX long on screen (shorter, they would crowd the
    // corners) - when its projection passes within `reach` pixels of the cursor.  A line
    // running behind the eye cannot be projected whole: then each point that projects must
    // itself lie within `reach`.
    template <class F> void LineDivisions( const edgeLine_t &L, float curX, float curY, float reach, F fn )
    {
        float a[3], b[3];
        ChainPoint( L, L.t0, a );
        ChainPoint( L, L.t1, b );
        float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f, t = 0.0f;
        const bool whole = Pick_WorldToImage( a, &ax, &ay ) && Pick_WorldToImage( b, &bx, &by );
        if ( whole && Pick_SegDist2D( curX, curY, ax, ay, bx, by, &t ) > reach )
            return;
        divPt_t d;
        Copy3( L.dir, d.dir );
        static const float FRACS[3] = { 0.5f, 0.25f, 0.75f };
        d.segPx = whole ? PixelDist( ax, ay, bx, by ) : KSNAP_QUARTER_MIN_PX;
        const int n = d.segPx >= KSNAP_QUARTER_MIN_PX ? 3 : 1;
        for ( int i = 0; i < n; ++i )
        {
            ChainPoint( L, L.t0 + ( L.t1 - L.t0 ) * FRACS[i], d.pos );
            if ( !Pick_WorldToImage( d.pos, &d.px, &d.py )
              || ( !whole && PixelDist( d.px, d.py, curX, curY ) > reach ) )
                continue;
            d.quarter = i > 0;
            fn( d );
        }
    }

    // What a live transform must not snap to (PICKF_EXCLUDE_SELECTED): its own brushes.
    // Sorted (KIWI 2026-10-11): every scan asks it once per map brush, and a linear list
    // made that selection x map per scan while moving a whole building.
    typedef std::vector<const selbrush_t *> exclSet_t;

    void GatherExcluded( unsigned pickFlags, exclSet_t &excluded )
    {
        if ( !( pickFlags & PICKF_EXCLUDE_SELECTED ) )
            return;
        for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
            excluded.push_back( b );
        const selection_t &selection = KiwiSel();
        for ( const sel_item_t &item : selection.items ) excluded.push_back( item.brush );
        std::sort( excluded.begin(), excluded.end(), std::less<const selbrush_t *>() );
    }

    bool Excluded( const exclSet_t &excluded, const selbrush_t *b )
    {
        return !excluded.empty()
            && std::binary_search( excluded.begin(), excluded.end(), b, std::less<const selbrush_t *>() );
    }

    // ═════════════════════════════════════════════════════════════════════════
    //  KIWI (2026-10-21, D-XRAY) — LINE OF SIGHT
    // ═════════════════════════════════════════════════════════════════════════
    // User: "the snapping points will only snap to points visible unless x-ray is turned
    // on, then anything."  The query's surface gate (CandGate below) only compares a
    // candidate with the ONE surface under the cursor: a corner hidden behind a pillar
    // that is not under the cursor, or any point while the cursor is over the void,
    // passed it.  So with x-ray OFF every candidate also needs a clear line from the eye.
    //
    // Cost: candidates all lie within a few pixels of the cursor ray, so anything that can
    // hide one lies in a narrow cone around that ray.  The cone's occluders are gathered
    // ONCE per query (lazily - a query that returns early pays nothing) and each test is a
    // segment against those few: convex brush slabs, patch render triangles (row boxes
    // first), xmodel meshes through the drop trace's cached trees.  Callers only test a
    // candidate that would beat the current best, so a query makes a handful of tests.
    const float KSNAP_LOS_PAD_PX   = 14.0f;  // cone half-width beyond a candidate's reach, px
    const float KSNAP_LOS_SLOP_PX  = 2.5f;   // a hit this close in front of the point still sees it
    const float KSNAP_LOS_SLOP_MIN = 0.25f;  // WORLD UNITS floor of that slack
    const float KSNAP_LOS_INSET    = 0.01f;  // WORLD UNITS a brush is shrunk by (grazing rays)
    const int   KSNAP_LOS_MAX      = 1024;   // occluders kept per gather

    struct losOcc_t
    {
        selbrush_t *node;
        int         kind;                    // 0 convex brush, 1 patch, 2 xmodel
        float       mins[3], maxs[3];
        int         rowFirst;                // patch: first row box in s_los.rows
    };

    struct losState_t
    {
        bool     armed     = false;          // this query tests line of sight
        bool     built     = false;
        ray_t    ray;
        unsigned pickFlags = 0;
        float    padPx     = 0.0f;
        std::vector<losOcc_t> occ;
        std::vector<float>    rows;          // 6 floats (mins, maxs) per patch cell row
    };
    losState_t s_los;

    // Pad from the box's FARTHEST corner: perspective world-per-pixel grows with depth, so
    // this never undercuts a candidate ray that clips the box's far end.
    bool BoxNearRay( const float *mins, const float *maxs, const ray_t &ray, float padPx )
    {
        float farC[3];
        for ( int k = 0; k < 3; ++k )
            farC[k] = ray.dir[k] >= 0.0f ? maxs[k] : mins[k];
        const float pad = padPx * KiwiCam_WorldPerPixel( farC );
        float lo = 0.0f, hi = 1.0e30f;
        for ( int k = 0; k < 3; ++k )
        {
            const float a = mins[k] - pad, b = maxs[k] + pad;
            if ( fabsf( ray.dir[k] ) < 1.0e-8f )
            {
                if ( ray.origin[k] < a || ray.origin[k] > b ) return false;
                continue;
            }
            float t0 = ( a - ray.origin[k] ) / ray.dir[k], t1 = ( b - ray.origin[k] ) / ray.dir[k];
            if ( t0 > t1 ) { const float t = t0; t0 = t1; t1 = t; }
            if ( t0 > lo ) lo = t0;
            if ( t1 < hi ) hi = t1;
            if ( lo > hi ) return false;
        }
        return true;
    }

    // Does the segment s + dir * (0..len) touch the box?
    bool SegBox( const float *s, const float *dir, float len, const float *mins, const float *maxs )
    {
        float lo = 0.0f, hi = len;
        for ( int k = 0; k < 3; ++k )
        {
            if ( fabsf( dir[k] ) < 1.0e-8f )
            {
                if ( s[k] < mins[k] || s[k] > maxs[k] ) return false;
                continue;
            }
            float t0 = ( mins[k] - s[k] ) / dir[k], t1 = ( maxs[k] - s[k] ) / dir[k];
            if ( t0 > t1 ) { const float t = t0; t0 = t1; t1 = t; }
            if ( t0 > lo ) lo = t0;
            if ( t1 < hi ) hi = t1;
            if ( lo > hi ) return false;
        }
        return true;
    }

    // The camera draws these tool volumes see-through (camwnd.cpp's editor-colour table
    // minus the two SOLID tools, caulk and nodraw, and sky), so they hide nothing.
    bool FaceSeeThrough( const face_t &f )
    {
        const qtexture_s *t = f.mtldef[g_qeglobals.current_edit_layer].radMtl;
        const char *name = t ? t->name : nullptr;
        if ( !name )
            return false;
        const char *slash = strrchr( name, '/' );
        const char *n = slash ? slash + 1 : name;
        static const char *const kSeeThrough[] =
            { "portal", "hint", "skip", "clip", "trigger", "origin", "volume", "lightgrid" };
        for ( const char *key : kSeeThrough )
            if ( strstr( n, key ) )
                return true;
        return false;
    }

    // Slab clip against every face plane of the brush shrunk by KSNAP_LOS_INSET.  A start
    // already inside (the eye in a brush) never occludes: the camera sees out of it.
    bool LosHitsBrush( const brush_t *def, const float *s, const float *dir, float len )
    {
        float t0 = 0.0f, t1 = len;
        int   enter = -1;
        for ( int f = 0; f < def->faceCount; ++f )
        {
            const plane_t &pl = def->faces[f].plane;
            const float num = Dot3( pl.normal, s ) - pl.dist + KSNAP_LOS_INSET;   // > 0 = outside
            const float den = Dot3( pl.normal, dir );
            if ( fabsf( den ) < 1.0e-9f )
            {
                if ( num > 0.0f )
                    return false;                    // parallel and outside this plane
                continue;
            }
            const float t = -num / den;
            if ( den < 0.0f )
            {
                if ( t > t0 ) { t0 = t; enter = f; }
            }
            else if ( t < t1 )
                t1 = t;
            if ( t0 >= t1 )
                return false;
        }
        return enter >= 0 && !FaceSeeThrough( def->faces[enter] );
    }

    // Two-sided: a patch is drawn from both sides.
    bool SegTri( const float *s, const float *dir, float len,
                 const float *v0, const float *v1, const float *v2 )
    {
        float e1[3], e2[3], p[3], q[3], tv[3];
        Sub3( v1, v0, e1 );
        Sub3( v2, v0, e2 );
        Cross3( dir, e2, p );
        const float det = Dot3( e1, p );
        if ( fabsf( det ) < 1.0e-9f )
            return false;
        const float inv = 1.0f / det;
        Sub3( s, v0, tv );
        const float u = Dot3( tv, p ) * inv;
        if ( u < 0.0f || u > 1.0f )
            return false;
        Cross3( tv, e1, q );
        const float v = Dot3( dir, q ) * inv;
        if ( v < 0.0f || u + v > 1.0f )
            return false;
        const float t = Dot3( e2, q ) * inv;
        return t > 1.0e-3f && t < len;
    }

    bool LosHitsPatch( const losOcc_t &o, const float *s, const float *dir, float len )
    {
        const patchMesh_t *pm = o.node->def->patch;
        const curvePatchDef_t *g = pm ? pm->curveDef : nullptr;
        if ( !g || !g->verts || g->width < 2 || g->height < 2 )
            return false;
        for ( int y = 0; y + 1 < g->height; ++y )
        {
            const float *rb = &s_los.rows[(size_t)( o.rowFirst + y ) * 6];
            if ( !SegBox( s, dir, len, rb, rb + 3 ) )
                continue;
            for ( int x = 0; x + 1 < g->width; ++x )
            {
                const float *v00 = g->verts[y * g->width + x].xyz;
                const float *v10 = g->verts[y * g->width + x + 1].xyz;
                const float *v01 = g->verts[( y + 1 ) * g->width + x].xyz;
                const float *v11 = g->verts[( y + 1 ) * g->width + x + 1].xyz;
                if ( SegTri( s, dir, len, v00, v10, v11 ) || SegTri( s, dir, len, v00, v11, v01 ) )
                    return true;
            }
        }
        return false;
    }

    // Arm (or disarm) the test for one query / one accent frame.  Cheap: the gather waits for
    // the first candidate that needs it.
    void LosBegin( const ray_t &ray, unsigned pickFlags, float reachPx )
    {
        s_los.armed     = !KiwiView_XRay();
        s_los.built     = false;
        s_los.ray       = ray;
        s_los.pickFlags = pickFlags;
        s_los.padPx     = reachPx + KSNAP_LOS_PAD_PX;
    }

    // What can hide a snap point is what the camera draws: unfiltered brushes and patches,
    // and xmodels by their mesh (point-entity boxes, prefab proxies and the geometry a live
    // transform is moving do not occlude).
    void LosBuild()
    {
        s_los.built = true;
        s_los.occ.clear();
        s_los.rows.clear();
        exclSet_t excluded;
        GatherExcluded( s_los.pickFlags, excluded );
        selbrush_t *lists[] = { &active_brushes, &selected_brushes };
        for ( selbrush_t *head : lists )
        for ( selbrush_t *b = head->next; b && b != head; b = b->next )
        {
            if ( (int)s_los.occ.size() >= KSNAP_LOS_MAX )
                return;
            if ( !b->def || !b->owner || FilterBrush( b, 0 ) || Excluded( excluded, b ) )
                continue;
            losOcc_t o;
            o.node     = b;
            o.rowFirst = 0;
            if ( b->owner != world_entity )
            {
                const entity_s *od = b->owner->def;
                if ( !od || !od->eclass || ( od->eclass->classtype & ECLASS_PREFAB ) != 0 )
                    continue;
                if ( od->eclass->fixedsize )
                {
                    if ( !KiwiDrop_IsModelEntity( b ) || b->owner->prefab )
                        continue;
                    float mins[3], maxs[3], angles[3], origin[3], scale = 1.0f, rmin[3], rmax[3];
                    XModel *model = 0;
                    if ( !KiwiDrop_GetModelInfo( b, mins, maxs, angles, &scale, origin, &model ) || !model
                      || !KiwiDrop_TransformBounds( mins, maxs, angles, scale, rmin, rmax ) )
                        continue;
                    for ( int k = 0; k < 3; ++k )
                    {
                        o.mins[k] = origin[k] + rmin[k];
                        o.maxs[k] = origin[k] + rmax[k];
                    }
                    if ( !BoxNearRay( o.mins, o.maxs, s_los.ray, s_los.padPx ) )
                        continue;
                    o.kind = 2;
                    s_los.occ.push_back( o );
                    continue;
                }
            }
            const brush_t *def = b->def;
            Copy3( def->mins, o.mins );
            Copy3( def->maxs, o.maxs );
            if ( !BoxNearRay( o.mins, o.maxs, s_los.ray, s_los.padPx ) )
                continue;
            if ( def->patch )
            {
                const curvePatchDef_t *g = def->patch->curveDef;
                if ( !g || !g->verts || g->width < 2 || g->height < 2 )
                    continue;
                o.kind     = 1;
                o.rowFirst = (int)( s_los.rows.size() / 6 );
                for ( int y = 0; y + 1 < g->height; ++y )
                {
                    float lo[3] = {  1.0e30f,  1.0e30f,  1.0e30f };
                    float hi[3] = { -1.0e30f, -1.0e30f, -1.0e30f };
                    for ( int r = y; r <= y + 1; ++r )
                        for ( int x = 0; x < g->width; ++x )
                        {
                            const float *p = g->verts[r * g->width + x].xyz;
                            for ( int k = 0; k < 3; ++k )
                            {
                                lo[k] = p[k] < lo[k] ? p[k] : lo[k];
                                hi[k] = p[k] > hi[k] ? p[k] : hi[k];
                            }
                        }
                    for ( int k = 0; k < 3; ++k )
                        s_los.rows.push_back( lo[k] - 0.01f );
                    for ( int k = 0; k < 3; ++k )
                        s_los.rows.push_back( hi[k] + 0.01f );
                }
                s_los.occ.push_back( o );
                continue;
            }
            if ( !def->faces || def->faceCount <= 0 )
                continue;
            o.kind = 0;
            s_los.occ.push_back( o );
        }
    }

    // Can the camera see p?  `ignore` never occludes (a cylinder's own shell around its
    // axis centre).  True whenever the test is not armed (x-ray, or no query context).
    bool LosVisible( const float *p, const selbrush_t *ignore = nullptr )
    {
        if ( !s_los.armed || !p )
            return true;
        if ( !KiwiSection_PointVisible( p ) )
            return false;                            // cut away by the section
        float s[3], dir[3], dist;
        if ( KiwiCam_Ortho() )
        {
            // Parallel rays: the point's own pixel ray, from the pick plane.
            Copy3( s_los.ray.dir, dir );
            float rel[3];
            Sub3( p, s_los.ray.origin, rel );
            dist = Dot3( rel, dir );
            if ( !( dist > 0.0f ) )
                return true;
            Mad3( p, dir, -dist, s );
        }
        else
        {
            Copy3( s_los.ray.origin, s );            // the eye
            Sub3( p, s, dir );
            dist = sqrtf( Dot3( dir, dir ) );
            if ( !( dist > 1.0e-4f ) )
                return true;
            for ( int k = 0; k < 3; ++k )
                dir[k] /= dist;
        }
        const float slopPx = KSNAP_LOS_SLOP_PX * KiwiCam_WorldPerPixel( p );
        float len = dist - ( slopPx > KSNAP_LOS_SLOP_MIN ? slopPx : KSNAP_LOS_SLOP_MIN );
        {
            // Geometry above an active section cut is not drawn, so it hides nothing.
            float s0[3];
            Copy3( s, s0 );
            KiwiSection_ClampRayStart( s, dir );
            float moved[3];
            Sub3( s, s0, moved );
            len -= Dot3( moved, dir );
        }
        if ( !( len > 0.0f ) )
            return true;
        if ( !s_los.built )
            LosBuild();
        for ( const losOcc_t &o : s_los.occ )
        {
            if ( o.node == ignore || !SegBox( s, dir, len, o.mins, o.maxs ) )
                continue;
            if ( o.kind == 0 )
            {
                if ( LosHitsBrush( o.node->def, s, dir, len ) )
                    return false;
            }
            else if ( o.kind == 1 )
            {
                if ( LosHitsPatch( o, s, dir, len ) )
                    return false;
            }
            else
            {
                float d = 0.0f;
                if ( KiwiDrop_TraceModelCached( o.node, s, dir, &d, nullptr ) == 1 && d > 1.0e-3f && d < len )
                    return false;
            }
        }
        return true;
    }

    // Faces turned away from the eye seed no edge targets (a brush's own back edges), except
    // in x-ray, where hidden geometry is fair game.
    inline bool BackFacing( const float *normal, const float *dir )
    {
        return Dot3( normal, dir ) >= 0.0f && !KiwiView_XRay();
    }

    // GrowLine's segment sources, one kind each: brush edges never join construction lines.
    // The brushes that may contribute are gathered ONCE per BrushDivisions into a flat array
    // (KIWI 2026-10-11, profiled 9% of a drag frame: every growth pass walked the map's list
    // and re-ran the pickable / proxy / excluded tests on every brush).
    struct BrushSegments
    {
        const std::vector<const brush_t *> *defs;
        template <class G> void operator()( const float *lo, const float *hi, G visit ) const
        {
            for ( const brush_t *def : *defs )
            {
                if ( def->maxs[0] < lo[0] || def->mins[0] > hi[0]
                  || def->maxs[1] < lo[1] || def->mins[1] > hi[1]
                  || def->maxs[2] < lo[2] || def->mins[2] > hi[2] )
                    continue;
                for ( int f = 0; f < def->faceCount; ++f )
                {
                    const winding_t *w = def->faces[f].w;
                    if ( !w || w->numpoints < 2 || w->numpoints > MAX_POINTS_ON_WINDING )
                        continue;
                    for ( int i = 0; i < w->numpoints; ++i )
                        visit( w->p[i], w->p[( i + 1 ) % w->numpoints] );
                }
            }
        }
    };

    // Straight construction geometry only: a circle / arc offers its centre and quadrants
    // (KiwiCon points) and a spline chain's tessellation segments have no meaningful middle.
    bool ConStraight( const kconObject_t *o )
    {
        return !KiwiCon_IsParametric( *o ) && !KiwiCon_HasSmooth( *o );
    }

    struct ConSegments
    {
        template <class G> void operator()( const float *lo, const float *hi, G visit ) const
        {
            const int count = KiwiCon_Count();
            for ( int i = 0; i < count; ++i )
            {
                const kconObject_t *o = KiwiCon_At( i );
                if ( !ConCandidateUsable( o, i ) || !ConStraight( o ) )
                    continue;
                const int segs = KiwiCon_SegmentCount( *o );
                for ( int s = 0; s < segs; ++s )
                {
                    float wa[3], wb[3];
                    if ( !KiwiCon_SegmentWorld( *o, s, wa, wb ) )
                        break;
                    bool outside = false;
                    for ( int k = 0; k < 3 && !outside; ++k )
                        outside = ( wa[k] < lo[k] && wb[k] < lo[k] ) || ( wa[k] > hi[k] && wb[k] > hi[k] );
                    if ( !outside )
                        visit( wa, wb );
                }
            }
        }
    };

    // Is segment p-q already part of one of `lines` (grown earlier in this query)?
    bool Covered( const std::vector<edgeLine_t> &lines, const float *p, const float *q )
    {
        for ( const edgeLine_t &L : lines )
        {
            float ta, tb;
            if ( OnLine( L, p, q, &ta, &tb )
              && ( ta < tb ? ta : tb ) >= L.t0 - KSNAP_LINE_GAP
              && ( ta < tb ? tb : ta ) <= L.t1 + KSNAP_LINE_GAP )
                return true;
        }
        return false;
    }

    // Is segment p-q within `reach` pixels of the cursor (or not projectable whole, which
    // LineDivisions settles per point)?
    bool SegmentAimed( const float *p, const float *q, float curX, float curY, float reach )
    {
        float px, py, qx, qy, t;
        return !( Pick_WorldToImage( p, &px, &py ) && Pick_WorldToImage( q, &qx, &qy )
               && Pick_SegDist2D( curX, curY, px, py, qx, qy, &t ) > reach );
    }

    // fn( const divPt_t &, selbrush_t *, int face, int edge ) over the division points of the
    // LINE of every pickable brush edge within `reach` pixels of the cursor (patches have no
    // winding edges - Pick offers their control points only); the node/face/edge is the aimed
    // piece.  Faces turned away from the cursor ray seed nothing: the nubs draw after a depth
    // clear, and a brush's own back edges would show through it (every visible edge,
    // silhouettes too, borders a front face).  Each line is emitted once.
    template <class F> void BrushDivisions( const ray_t &ray, float curX, float curY, float reach,
                                            unsigned pickFlags, F fn )
    {
        exclSet_t excluded;
        GatherExcluded( pickFlags, excluded );
        std::vector<edgeLine_t> lines;
        std::vector<const brush_t *> segDefs;       // GrowLine's sources (BrushSegments)
        bool haveSegDefs = false;
        selbrush_t *lists[] = { &active_brushes, &selected_brushes };
        for ( selbrush_t *head : lists )
        for ( selbrush_t *b = head->next; b && b != head; b = b->next )
        {
            if ( b->patch || Excluded( excluded, b ) || !Pick_BrushPickable( b ) || Pick_IsModelProxy( b ) )
                continue;
            const brush_t *def = b->def;
            if ( !def || !def->faces || def->faceCount <= 0 || !BoundsNearRay( def, ray, reach * 2.0f ) )
                continue;
            if ( !haveSegDefs )
            {
                haveSegDefs = true;
                for ( selbrush_t *h2 : lists )
                for ( selbrush_t *s = h2->next; s && s != h2; s = s->next )
                    if ( s->def && s->def->faces && !s->patch && !Excluded( excluded, s )
                      && Pick_BrushPickable( s ) && !Pick_IsModelProxy( s ) )
                        segDefs.push_back( s->def );
            }
            for ( int f = 0; f < def->faceCount; ++f )
            {
                const winding_t *w = def->faces[f].w;
                if ( !w || w->numpoints < 2 || w->numpoints > MAX_POINTS_ON_WINDING
                  || BackFacing( def->faces[f].plane.normal, ray.dir ) )   // KIWI: x-ray takes back faces
                    continue;
                for ( int i = 0; i < w->numpoints; ++i )
                {
                    const float *p = w->p[i];
                    const float *q = w->p[( i + 1 ) % w->numpoints];
                    edgeLine_t   L;
                    if ( !SegmentAimed( p, q, curX, curY, reach ) || Covered( lines, p, q )
                      || !SeedLine( L, p, q ) )
                        continue;
                    GrowLine( L, BrushSegments{ &segDefs } );
                    lines.push_back( L );
                    LineDivisions( lines.back(), curX, curY, reach,
                                   [&]( const divPt_t &d ) { fn( d, b, f, i ); } );
                }
            }
        }
    }

    // fn( const divPt_t & ) over the division points of the LINE of every usable, straight
    // construction segment within `reach` pixels of the cursor (lines as above; each once).
    template <class F> void ConDivisions( float curX, float curY, float reach, F fn )
    {
        std::vector<edgeLine_t> lines;
        const int count = KiwiCon_Count();
        for ( int i = 0; i < count; ++i )
        {
            const kconObject_t *o = KiwiCon_At( i );
            if ( !ConCandidateUsable( o, i ) || !ConStraight( o ) )
                continue;
            const int segs = KiwiCon_SegmentCount( *o );
            for ( int s = 0; s < segs; ++s )
            {
                float      wa[3], wb[3];
                edgeLine_t L;
                if ( !KiwiCon_SegmentWorld( *o, s, wa, wb ) )
                    break;
                if ( !SegmentAimed( wa, wb, curX, curY, reach ) || Covered( lines, wa, wb )
                  || !SeedLine( L, wa, wb ) )
                    continue;
                GrowLine( L, ConSegments() );
                lines.push_back( L );
                LineDivisions( lines.back(), curX, curY, reach, fn );
            }
        }
    }

    // Construction midpoints (and quarter points) use the 7 SCREEN-PIXEL point radius and
    // outrank brush ones and all line snaps.
    void ScanConMidpoints( float curX, float curY, conBest_t *best )
    {
        ConDivisions( curX, curY, KSNAP_R_CON_POINT, [&]( const divPt_t &d )
        {
            const float dist = PixelDist( d.px, d.py, curX, curY );
            if ( dist > KSNAP_R_CON_POINT || ( best->hit && dist >= best->dist ) )
                return;
            best->hit     = true;
            best->dist    = dist;
            best->haveDir = true;
            best->quarter = d.quarter;
            Copy3( d.pos, best->pos );
            Copy3( d.dir, best->dir );
        } );
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

    // Where segments a and b cross ON SCREEN, as a's and b's fractions.  Used in orthographic
    // views only, where a screen fraction is the world fraction.
    bool ScreenCross( const float *a0, const float *a1, const float *b0, const float *b1,
                      float *outS, float *outT )
    {
        float ax, ay, bx, by, cx, cy, dx, dy;
        if ( !Pick_WorldToImage( a0, &ax, &ay ) || !Pick_WorldToImage( a1, &bx, &by )
          || !Pick_WorldToImage( b0, &cx, &cy ) || !Pick_WorldToImage( b1, &dx, &dy ) )
            return false;
        const float rx = bx - ax, ry = by - ay, sx = dx - cx, sy = dy - cy;
        const float den = rx * sy - ry * sx;
        if ( fabsf( den ) < 1.0e-6f )
            return false;
        const float qx = cx - ax, qy = cy - ay;
        const float s  = ( qx * sy - qy * sx ) / den;
        const float t  = ( qx * ry - qy * rx ) / den;
        if ( s < -1.0e-4f || s > 1.0001f || t < -1.0e-4f || t > 1.0001f )
            return false;
        *outS = s;
        *outT = t;
        return true;
    }

    // KIWI (2026-09-26, user drawing a roof profile on a cube: "it should allow me to snap where
    // the line intersects the edge of the cube"): construction segment x brush edge crossings,
    // by 3D closest approach like the construction pairs above.  The gap allowed is
    // KCON_ISECT_DIST or one screen pixel at the crossing, whichever is larger (float noise at
    // big map coordinates).  In an orthographic view a crossing ON SCREEN also counts (the
    // line may sit on a construction plane in front of or behind the face - 2D drafting).
    // Only segments and edges passing within the point radius of the cursor pair up, and only
    // edges of faces turned to the eye; the query's surface gate is the occlusion policy (a
    // planar placer's own plane is never occluded, so a screen crossing also needs the EDGE's
    // point to pass `visible`).  The point lies ON the construction segment.
    template <class G>
    void ScanConBrushCrossings( const ray_t &ray, float curX, float curY, unsigned pickFlags,
                                const G &visible, conBest_t *best )
    {
        const bool ortho = KiwiCam_Ortho();
        enum { KSNAP_MAX_XSEGS = 64 };
        float con[KSNAP_MAX_XSEGS][2][3];
        int   nCon = 0;
        const int count = KiwiCon_Count();
        for ( int i = 0; i < count && nCon < KSNAP_MAX_XSEGS; ++i )
        {
            const kconObject_t *o = KiwiCon_At( i );
            if ( !ConCandidateUsable( o, i ) )
                continue;
            const int n = KiwiCon_SegmentCount( *o );
            for ( int s = 0; s < n && nCon < KSNAP_MAX_XSEGS; ++s )
            {
                if ( !KiwiCon_SegmentWorld( *o, s, con[nCon][0], con[nCon][1] ) )
                    break;
                if ( SegmentAimed( con[nCon][0], con[nCon][1], curX, curY, KSNAP_R_CON_POINT ) )
                    ++nCon;
            }
        }
        if ( nCon == 0 )
            return;

        exclSet_t excluded;
        GatherExcluded( pickFlags, excluded );
        selbrush_t *lists[] = { &active_brushes, &selected_brushes };
        for ( selbrush_t *head : lists )
        for ( selbrush_t *b = head->next; b && b != head; b = b->next )
        {
            if ( b->patch || Excluded( excluded, b ) || !Pick_BrushPickable( b ) || Pick_IsModelProxy( b ) )
                continue;
            const brush_t *def = b->def;
            if ( !def || !def->faces || def->faceCount <= 0
              || !BoundsNearRay( def, ray, KSNAP_R_CON_POINT * 2.0f ) )
                continue;
            for ( int f = 0; f < def->faceCount; ++f )
            {
                const winding_t *w = def->faces[f].w;
                if ( !w || w->numpoints < 2 || w->numpoints > MAX_POINTS_ON_WINDING
                  || BackFacing( def->faces[f].plane.normal, ray.dir ) )   // KIWI: x-ray takes back faces
                    continue;
                for ( int i = 0; i < w->numpoints; ++i )
                {
                    const float *p = w->p[i];
                    const float *q = w->p[( i + 1 ) % w->numpoints];
                    if ( !SegmentAimed( p, q, curX, curY, KSNAP_R_CON_POINT ) )
                        continue;
                    float e[3];
                    Sub3( q, p, e );
                    const float le = sqrtf( Dot3( e, e ) );
                    if ( !( le > 1.0e-4f ) )
                        continue;
                    for ( int c = 0; c < nCon; ++c )
                    {
                        float dc[3], cr[3];
                        Sub3( con[c][1], con[c][0], dc );
                        Cross3( dc, e, cr );
                        const float lc = sqrtf( Dot3( dc, dc ) );
                        // Parallel / overlapping: no single crossing (the edge snap covers it).
                        if ( !( lc > 1.0e-4f ) || sqrtf( Dot3( cr, cr ) ) / ( lc * le ) < 1.0e-3f )
                            continue;
                        float ta = 0.0f, tb = 0.0f, mid[3], on[3];
                        const float gap = KiwiCon_SegSegClosest( con[c][0], con[c][1], p, q,
                                                                 &ta, &tb, mid );
                        for ( int k = 0; k < 3; ++k )
                            on[k] = con[c][0][k] + dc[k] * ta;
                        const float pixTol = KiwiCam_WorldPerPixel( on );
                        if ( gap > ( pixTol > KCON_ISECT_DIST ? pixTol : KCON_ISECT_DIST ) )
                        {
                            float s, t, onEdge[3];     // they pass each other in depth...
                            if ( !ortho || !ScreenCross( con[c][0], con[c][1], p, q, &s, &t ) )
                                continue;
                            for ( int k = 0; k < 3; ++k )  // ...but cross on an ortho screen
                            {
                                on[k]     = con[c][0][k] + dc[k] * s;
                                onEdge[k] = p[k] + e[k] * t;
                            }
                            if ( !visible( onEdge ) )
                                continue;              // an edge hidden behind the surface
                        }
                        float px, py;
                        if ( !Pick_WorldToImage( on, &px, &py ) )
                            continue;
                        const float d = PixelDist( px, py, curX, curY );
                        if ( d > KSNAP_R_CON_POINT || ( best->hit && d >= best->dist ) )
                            continue;
                        best->hit  = true;
                        best->dist = d;
                        Copy3( on, best->pos );
                    }
                }
            }
        }
    }

    // KIWI (2026-10-11, user: "missing snap points ... where these 2 brushes intersect"): where
    // an edge of one brush passes through a face of another (a post sunk into a sill).  Flush
    // faces need no edge x edge case: an edge lying in one face of the other brush crosses the
    // neighbouring face on its border, which the face test takes with KSNAP_XB_BORDER slack.
    // An edge ending on the plane is a corner (rank 2), so it must cross by KSNAP_XB_PLANE.
    // Only edges of faces turned to the eye, and faces turned to it, take part (like the nubs).
    const float KSNAP_XB_PLANE   = 0.05f;   // WORLD UNITS each end lies past the face plane
    const float KSNAP_XB_BORDER  = 0.1f;    // WORLD UNITS a crossing may lie outside the face
    const int   KSNAP_XB_BRUSHES = 64;      // brushes near the ray paired per query

    // Does x, on face's plane, lie inside its winding (within KSNAP_XB_BORDER)?
    bool InsideFace( const face_t &face, const float *n, const float *x )
    {
        const winding_t *w = face.w;
        float lo = 0.0f, hi = 0.0f;
        for ( int i = 0; i < w->numpoints; ++i )
        {
            float e[3], r[3], c[3];
            Sub3( w->p[( i + 1 ) % w->numpoints], w->p[i], e );
            Sub3( x, w->p[i], r );
            Cross3( e, r, c );
            const float le = sqrtf( Dot3( e, e ) );
            if ( le < 1.0e-4f )
                continue;
            const float s = Dot3( c, n ) / le;      // signed distance from the edge's line
            lo = s < lo ? s : lo;
            hi = s > hi ? s : hi;
        }
        return lo >= -KSNAP_XB_BORDER || hi <= KSNAP_XB_BORDER;
    }

    // fn( pos, px, py ) for each brush edge x brush face crossing whose edge passes within
    // `reach` pixels of the cursor.
    template <class F> void BrushCrossings( const ray_t &ray, float curX, float curY, float reach,
                                            unsigned pickFlags, F fn )
    {
        exclSet_t excluded;
        GatherExcluded( pickFlags, excluded );
        const brush_t *cands[KSNAP_XB_BRUSHES];
        int nCands = 0;
        selbrush_t *lists[] = { &active_brushes, &selected_brushes };
        for ( selbrush_t *head : lists )
        for ( selbrush_t *b = head->next; b && b != head && nCands < KSNAP_XB_BRUSHES; b = b->next )
        {
            if ( b->patch || Excluded( excluded, b ) || !Pick_BrushPickable( b ) || Pick_IsModelProxy( b ) )
                continue;
            const brush_t *def = b->def;
            if ( def && def->faces && def->faceCount > 0 && BoundsNearRay( def, ray, reach * 2.0f ) )
                cands[nCands++] = def;
        }
        for ( int a = 0; a < nCands; ++a )
        for ( int b = 0; b < nCands; ++b )
        {
            const brush_t *A = cands[a], *B = cands[b];
            if ( a == b
              || A->maxs[0] < B->mins[0] || A->mins[0] > B->maxs[0]
              || A->maxs[1] < B->mins[1] || A->mins[1] > B->maxs[1]
              || A->maxs[2] < B->mins[2] || A->mins[2] > B->maxs[2] )
                continue;
            for ( int f = 0; f < A->faceCount; ++f )
            {
                const winding_t *w = A->faces[f].w;
                if ( !w || w->numpoints < 2 || w->numpoints > MAX_POINTS_ON_WINDING
                  || BackFacing( A->faces[f].plane.normal, ray.dir ) )   // KIWI: x-ray takes back faces
                    continue;
                for ( int i = 0; i < w->numpoints; ++i )
                {
                    const float *p = w->p[i];
                    const float *q = w->p[( i + 1 ) % w->numpoints];
                    if ( !SegmentAimed( p, q, curX, curY, reach ) )
                        continue;
                    for ( int g = 0; g < B->faceCount; ++g )
                    {
                        const face_t &face = B->faces[g];
                        if ( !face.w || face.w->numpoints < 3 || face.w->numpoints > MAX_POINTS_ON_WINDING )
                            continue;
                        float n[3];
                        for ( int k = 0; k < 3; ++k )
                            n[k] = (float)face.plane.normal[k];
                        if ( BackFacing( n, ray.dir ) )            // KIWI: x-ray takes back faces
                            continue;
                        const float d0 = Dot3( n, p ) - (float)face.plane.dist;
                        const float d1 = Dot3( n, q ) - (float)face.plane.dist;
                        if ( !( ( d0 > KSNAP_XB_PLANE && d1 < -KSNAP_XB_PLANE )
                             || ( d0 < -KSNAP_XB_PLANE && d1 > KSNAP_XB_PLANE ) ) )
                            continue;
                        const float t = d0 / ( d0 - d1 );
                        float x[3], px, py;
                        for ( int k = 0; k < 3; ++k )
                            x[k] = p[k] + ( q[k] - p[k] ) * t;
                        if ( InsideFace( face, n, x ) && Pick_WorldToImage( x, &px, &py )
                          && PixelDist( px, py, curX, curY ) <= reach )
                            fn( x, px, py );
                    }
                }
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
        const char *name = "ext";          // guide label unless the line is a world axis
    };
    // Insert by ascending key, deduplicate parallel directions, and drop the worst
    // entry at maxN. Equal keys prefer the later candidate.
    void AnchorDirPush( anchorDir_t *list, int *count, int maxN,
                        const float *dir, float d2, float key, const char *name = "ext" )
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
        list[at].d2   = d2;
        list[at].key  = key;
        list[at].name = name;
        if ( *count < maxN )
            ++*count;
    }
    // A line contains the anchor when point-to-segment distance is within
    // KREG_JOIN_DIST WORLD UNITS, the shared endpoint weld tolerance. With aim, rank
    // by collinearity; without aim, prefer the in-progress chain then nearest distance.
    //
    // KIWI (2026-09-21, user: "when drawing a multi point line at an angle, the angle
    // match should be one of the snapping points"): with `planeN` (aimed, free tools)
    // the open chain also offers, through the anchor, the PERPENDICULAR of its last
    // segment and the PARALLEL + PERPENDICULAR of every earlier one.  Only the last
    // segment's own line was a guide, and the 15-degree band (arm 5b) measures from the
    // world axes, so a chain started at 37 degrees had nothing to square its next leg
    // against.  Perpendiculars are taken in the frame's resolved plane.
    enum { KSNAP_MAX_CHAIN_SEGS = 32 };
    int GatherAnchorDirs( const float anchor[3], const float *aim,
                          anchorDir_t *out, int maxN, const float *planeN = nullptr )
    {
        int count = 0;
        if ( maxN < 1 )
            return 0;
        if ( aim && planeN )
        {
            const int pts  = KiwiCon_ToolChainCount();
            int       segs = 0;
            for ( int i = pts - 1; i >= 1 && segs < KSNAP_MAX_CHAIN_SEGS; --i, ++segs )
            {
                float a[3], b[3], dir[3];
                if ( !KiwiCon_ToolChainPoint( i - 1, a ) || !KiwiCon_ToolChainPoint( i, b ) )
                    break;
                Sub3( b, a, dir );
                if ( !Norm3( dir ) )
                    continue;
                // The last segment's own line is pushed below as the continuation.
                if ( i != pts - 1 )
                    AnchorDirPush( out, &count, maxN, dir, 0.0f,
                                   1.0f - fabsf( Dot3( dir, aim ) ), "par" );
                float perp[3];
                Cross3( planeN, dir, perp );
                if ( Norm3( perp ) )
                    AnchorDirPush( out, &count, maxN, perp, 0.0f,
                                   1.0f - fabsf( Dot3( perp, aim ) ), "perp" );
            }
        }
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
                if ( !Pick_BrushPickable( b ) || Pick_IsModelProxy( b ) )
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
    // KIWI (2026-10-21, D-XRAY): the nearest brush corner / patch control point (edges ==
    // false) or nearest brush-edge point (edges == true) within Pick's own radius that
    // `visible` passes.  Pick() keeps ONE winner per kind, so once line of sight rejects a
    // hidden winner nothing would be left; this ranks the alternatives nearest-on-screen
    // first with Pick's admission rules and tests at most KSNAP_ALT_TRY of them.
    enum { KSNAP_ALT_TRY = 24 };
    template <class G>
    bool NearestVisiblePoint( const ray_t &ray, float curX, float curY, unsigned pickFlags,
                              bool edges, const G &visible, pick_result_t *out )
    {
        struct altCand_t
        {
            float      d;
            float      p[3];
            sel_item_t item;
        };
        altCand_t c[KSNAP_ALT_TRY];
        int n = 0;
        const float reach = edges ? PICK_EDGE_PIXELS : PICK_VERT_PIXELS;
        auto offer = [&]( float d, const float *p, const sel_item_t &item )
        {
            if ( d > reach )
                return;
            int at = n;
            while ( at > 0 && c[at - 1].d > d )
                --at;
            if ( at >= KSNAP_ALT_TRY )
                return;
            const int last = n < KSNAP_ALT_TRY ? n : KSNAP_ALT_TRY - 1;
            for ( int i = last; i > at; --i )
                c[i] = c[i - 1];
            c[at].d = d;
            Copy3( p, c[at].p );
            c[at].item = item;
            if ( n < KSNAP_ALT_TRY )
                ++n;
        };
        if ( !Pick_ProjectBegin() )
            return false;
        exclSet_t excluded;
        GatherExcluded( pickFlags, excluded );
        selbrush_t *lists[] = { &active_brushes, &selected_brushes };
        for ( selbrush_t *head : lists )
        for ( selbrush_t *b = head->next; b && b != head; b = b->next )
        {
            if ( Excluded( excluded, b ) || !Pick_BrushPickable( b ) || Pick_IsModelProxy( b ) )
                continue;
            const brush_t *def = b->def;
            if ( !def || !BoundsNearRay( def, ray, reach * 2.0f ) )
                continue;
            if ( b->patch )
            {
                // Patches offer their control points only (kiwi_pick.cpp ScanList).
                const patchMesh_t *pm = def->patch;
                if ( edges || !pm || pm->width <= 0 || pm->height <= 0 || pm->width > 16 || pm->height > 16 )
                    continue;
                for ( int col = 0; col < pm->width; ++col )
                    for ( int row = 0; row < pm->height; ++row )
                    {
                        const float *xyz = pm->ctrl[col][row].xyz;
                        float sx, sy;
                        if ( Pick_Project( xyz, &sx, &sy ) )
                            offer( PixelDist( sx, sy, curX, curY ), xyz,
                                   Sel_MakePatchPoint( b, col * pm->height + row ) );
                    }
                continue;
            }
            if ( !def->faces )
                continue;
            for ( int f = 0; f < def->faceCount; ++f )
            {
                const winding_t *w = def->faces[f].w;
                if ( !w || w->numpoints < 1 || w->numpoints > MAX_POINTS_ON_WINDING )
                    continue;
                for ( int i = 0; i < w->numpoints; ++i )
                {
                    float ax, ay;
                    if ( !Pick_Project( w->p[i], &ax, &ay ) )
                        continue;
                    if ( !edges )
                    {
                        offer( PixelDist( ax, ay, curX, curY ), w->p[i], Sel_MakeVertex( b, f, i ) );
                        continue;
                    }
                    if ( w->numpoints < 3 )
                        continue;
                    const int j = ( i + 1 ) % w->numpoints;
                    float bx, by, t = 0.0f;
                    if ( !Pick_Project( w->p[j], &bx, &by ) )
                        continue;
                    const float d = Pick_SegDist2D( curX, curY, ax, ay, bx, by, &t );
                    t = Pick_ScreenToWorldT( w->p[i], w->p[j], t );   // KIWI (2026-10-26): perspective-correct
                    float mid[3];
                    for ( int k = 0; k < 3; ++k )
                        mid[k] = w->p[i][k] + ( w->p[j][k] - w->p[i][k] ) * t;
                    offer( d, mid, Sel_MakeEdge( b, f, i ) );
                }
            }
        }
        for ( int i = 0; i < n; ++i )
        {
            if ( !visible( c[i].p ) )
                continue;
            out->valid      = true;
            out->item       = c[i].item;
            out->screenDist = c[i].d;
            out->haveNormal = false;
            Copy3( c[i].p, out->point );
            return true;
        }
        return false;
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

bool KiwiSnap_LastEdgeDir( float out[3] )
{
    if ( !s_haveEdgeDir )
        return false;
    out[0] = s_edgeDir[0];
    out[1] = s_edgeDir[1];
    out[2] = s_edgeDir[2];
    return true;
}

const char *KiwiSnap_TypeName( snap_type_t t )
{
    switch ( t )
    {
    case SNAP_GRID:         return "grid";
    case SNAP_VERTEX:       return "vert";
    case SNAP_EDGE_MID:     return "mid";
    case SNAP_EDGE_QUARTER: return "quarter";
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
                            bool hard, bool *outMajor, bool absolute )
{
    if ( outMajor )
        *outMajor = false;
    // The master grid switch disables both hard quantization and the soft band.
    if ( !KiwiGrid_SnapEnabled() )
        return d;
    const float g = KiwiUnits_GridSpacingWorld();
    if ( !( g > 0.0f ) || !ref || !axis )
        return d;
    // KIWI (2026-10-14, user: "when nothing is being snapped to, I want the operations to
    // be rounded to the nearest gridsize ... it has a weird .342in extension instead of
    // being an even 6 per"): the gesture DISTANCE steps in whole grid cells.  Only the
    // absolute ladder (Ctrl depth) still snaps a world-axis coordinate onto the lattice.
    // scale converts a lattice correction back into gesture distance.
    float value = d;
    float scale = 1.0f;                 // d units per `value` unit
    int   worldAxis = -1;
    for ( int k = 0; k < 3 && absolute; ++k )
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
    // KIWI (2026-10-27, user: "If I draw a line like this, there is no construction face
    // made"): a free line drawn over a construction face landed on whatever brush lies
    // behind it, so a face that does not sit exactly on that brush (an inch proud of the
    // wall, tilted, or in the air) never received the line in its plane and was never cut.
    // A free draw tool now draws ON the construction face under the cursor, with the same
    // arbitration a click uses (KiwiRegion_WinsOverPick: a nearer solid still wins).
    bool  onRegion = false;
    float regionHit[3]    = { 0.0f, 0.0f, 0.0f };
    float regionNormal[3] = { 0.0f, 0.0f, 1.0f };
    if ( !haveCPlane && KiwiCon_FreeToolActive() && KiwiCon_ShowConstruction() )
    {
        const int ri = KiwiRegion_PickAt( ray );
        float     rd = 0.0f;
        if ( ri >= 0 && KiwiRegion_HitDistance( ray, ri, &rd )
          && ( !surf.valid || KiwiRegion_WinsOverPick( ray, ri, surf ) ) )
        {
            onRegion = true;
            RayPoint( ray, rd, regionHit );
            Copy3( KiwiRegion_All()[(size_t)ri].plane.normal, regionNormal );
        }
    }
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
    else if ( onRegion )
    {
        Copy3( regionHit, raw );
        rawRung = 4;                         // a construction face (KIWI 2026-10-27)
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
    else if ( rawRung == 4 )
        Copy3( regionNormal, out->planeNormal );
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
    // The candidate depth gates: the surface under the cursor, then (KIWI 2026-10-21,
    // D-XRAY) a clear line of sight from the eye.  X-ray lifts both: "then anything".
    // Construction geometry draws on top of the scene (the camera's overlay tail, after its
    // depth clear), so it is never hidden by a wall: construction candidates take Surf()
    // only, exactly as before.  Scene geometry (brush, patch, model points) takes the call.
    struct CandGate
    {
        bool         on;                 // surface occlusion enabled
        bool         los;                // line-of-sight test armed (x-ray off)
        float        surfT;
        const ray_t *ray;
        bool Surf( const float *p ) const
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
        // `ignore` neither occludes in the line-of-sight test; `skipSurf` drops the surface
        // gate (a cylinder's own shell under the cursor must not hide its axis centre).
        bool Scene( const float *p, const selbrush_t *ignore, bool skipSurf ) const
        {
            if ( !p )
                return true;
            if ( !skipSurf && !Surf( p ) )
                return false;
            return !los || LosVisible( p, ignore );
        }
        bool operator()( const float *p ) const
        {
            return Scene( p, nullptr, false );
        }
    };
    const bool xray = KiwiView_XRay();
    // Every candidate lies within PICK_VERT_PIXELS (8 px) of the query ray.
    LosBegin( ray, pickFlags, PICK_VERT_PIXELS );
    CandGate visible;
    visible.on      = haveSurfDepth && !xray;
    visible.los     = !xray;
    visible.surfT   = surfT;
    visible.ray     = &ray;
    // Gather each construction candidate class once.
    conBest_t conAnchor, conSeg, conIsect, conMid, conXBrush;
    if ( haveCursorPx && KiwiCon_Count() > 0 )
    {
        ScanConAnchors      ( curX, curY, &conAnchor );
        ScanConIntersections( curX, curY, &conIsect );
        ScanConBrushCrossings( ray, curX, curY, pickFlags, visible, &conXBrush );
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
    if ( conAnchor.hit && visible.Surf( conAnchor.pos ) )
    {
        out->valid = true;
        out->type  = SNAP_ENDPOINT;
        out->position[0] = conAnchor.pos[0];
        out->position[1] = conAnchor.pos[1];
        out->position[2] = conAnchor.pos[2];
        return true;                     // `source` stays the null item — see the header
    }
    // Point rank 2: brush corner, patch control point or xmodel mesh vertex (KIWI 2026-10-03),
    // 8 px and not occluded; the nearer on screen wins.
    pick_result_t vert = Pick( ray, SEL_MASK_VERTEX, pickFlags );
    bool brushVert = vert.valid && visible( vert.point );
    // KIWI (D-XRAY): Pick names only the NEAREST corner on screen; when that one is hidden,
    // the nearest one the camera can see within the same radius takes its place.
    if ( vert.valid && !brushVert && haveCursorPx )
    {
        pick_result_t alt;
        if ( NearestVisiblePoint( ray, curX, curY, pickFlags, false, visible, &alt ) )
        {
            vert      = alt;
            brushVert = true;
        }
    }
    kiwiModelVert_t modelVerts[KSNAP_MODEL_VERT_TRY];
    const int modelVertCount = haveCursorPx
        ? KiwiDrop_ModelVertsNear( curX, curY, KSNAP_R_VERTEX, ( pickFlags & PICKF_EXCLUDE_SELECTED ) != 0,
                                   modelVerts, KSNAP_MODEL_VERT_TRY )
        : 0;
    const kiwiModelVert_t *modelVert = 0;
    if ( modelVertCount > 0 )
    {
        // Object-mode Pick skips models that are not selectable; the camera's solid trace
        // sees every resident mesh, so model vertices answer to the nearer of the two.
        CandGate modelVisible = visible;
        float solidDist = 0.0f;
        if ( !xray && Pick_TraceSolid( ray.origin, ray.dir, &solidDist ) )   // KIWI: x-ray = no gate
        {
            modelVisible.on    = true;
            modelVisible.surfT = haveSurfDepth && visible.surfT < solidDist ? visible.surfT : solidDist;
            if ( haveCPlane )
            {
                const float rel[3] = { cplaneHit[0] - ray.origin[0], cplaneHit[1] - ray.origin[1],
                                       cplaneHit[2] - ray.origin[2] };
                const float t = Dot3( rel, ray.dir );
                if ( t > modelVisible.surfT )
                    modelVisible.surfT = t;      // a planar placer works on its own plane
            }
        }
        for ( int i = 0; i < modelVertCount && !modelVert; ++i )
            if ( modelVisible( modelVerts[i].pos ) )
                modelVert = &modelVerts[i];
    }
    if ( modelVert && brushVert )
    {
        float bx, by;
        if ( Pick_WorldToImage( vert.point, &bx, &by ) && PixelDist( bx, by, curX, curY ) <= modelVert->distPx )
            modelVert = 0;                       // the brush point is at least as near
    }
    if ( modelVert )
    {
        out->valid = true;
        out->type  = SNAP_VERTEX;                // `source` stays null: no brush item names it
        out->position[0] = modelVert->pos[0];
        out->position[1] = modelVert->pos[1];
        out->position[2] = modelVert->pos[2];
        // KIWI: the mesh triangle under the cursor that owns this vertex, for the wireframe;
        // several queries a frame at one cursor share one lookup.
        static int   s_meshTriFrame = -1;
        static float s_meshTriCur[2];
        const int frame = ImGui::GetCurrentContext() ? ImGui::GetFrameCount() : -2;
        const bool same = frame >= 0 && frame == s_meshTriFrame
                       && s_meshTriCur[0] == curX && s_meshTriCur[1] == curY
                       && s_meshTriAt[0] == modelVert->pos[0] && s_meshTriAt[1] == modelVert->pos[1]
                       && s_meshTriAt[2] == modelVert->pos[2];
        if ( !same )
        {
            s_meshTriValid = KiwiDrop_ModelVertTriangle( modelVert->node, modelVert->pos, curX, curY,
                                                         s_meshTri );
            s_meshTriFrame  = frame;
            s_meshTriCur[0] = curX;
            s_meshTriCur[1] = curY;
            for ( int k = 0; k < 3; ++k )
                s_meshTriAt[k] = modelVert->pos[k];
        }
        return true;
    }
    if ( brushVert )
    {
        out->valid  = true;
        out->type   = SNAP_VERTEX;
        out->source = vert.item;
        out->position[0] = vert.point[0];
        out->position[1] = vert.point[1];
        out->position[2] = vert.point[2];
        return true;
    }
    // Point rank 3: a construction line crossing another construction line or a brush edge,
    // or a brush edge through another brush's face (the nearer on screen), 7 px and not occluded.
    {
        conBest_t brushX;
        if ( haveCursorPx )
            BrushCrossings( ray, curX, curY, KSNAP_R_CON_POINT, pickFlags,
                            [&]( const float *x, float px, float py )
            {
                const float d = PixelDist( px, py, curX, curY );
                if ( ( brushX.hit && d >= brushX.dist ) || !visible( x ) )
                    return;
                brushX.hit  = true;
                brushX.dist = d;
                Copy3( x, brushX.pos );
            } );
        const conBest_t *isect = 0;
        if ( conIsect.hit && visible.Surf( conIsect.pos ) )
            isect = &conIsect;
        if ( conXBrush.hit && visible( conXBrush.pos ) && ( !isect || conXBrush.dist < isect->dist ) )
            isect = &conXBrush;
        if ( brushX.hit && ( !isect || brushX.dist < isect->dist ) )
            isect = &brushX;
        if ( isect )
        {
            out->valid = true;
            out->type  = SNAP_INTERSECTION;
            out->position[0] = isect->pos[0];
            out->position[1] = isect->pos[1];
            out->position[2] = isect->pos[2];
            return true;
        }
    }
    // Point rank 3b: construction midpoint or quarter point, 7 px and not occluded.
    if ( conMid.hit && visible.Surf( conMid.pos ) )
    {
        out->valid = true;
        out->type  = conMid.quarter ? SNAP_EDGE_QUARTER : SNAP_EDGE_MID;
        out->position[0] = conMid.pos[0];
        out->position[1] = conMid.pos[1];
        out->position[2] = conMid.pos[2];
        s_haveEdgeDir = conMid.haveDir;
        if ( conMid.haveDir )
            for ( int k = 0; k < 3; ++k )
                s_edgeDir[k] = conMid.dir[k];
        return true;                     // `source` stays null — see the header
    }
    // One brush-edge Pick supplies the line-rank point and its direction.
    pick_result_t edge = Pick( ray, SEL_MASK_EDGE, pickFlags );
    // KIWI (D-XRAY): as for corners, a hidden nearest edge yields to the nearest visible one.
    if ( edge.valid && haveCursorPx && !visible( edge.point ) )
    {
        pick_result_t alt;
        if ( NearestVisiblePoint( ray, curX, curY, pickFlags, true, visible, &alt ) )
            edge = alt;
    }
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
        }
    }
    // Point rank 4: the nearest brush-edge midpoint or quarter point within 7 SCREEN PIXELS,
    // over EVERY edge near the cursor, each taken over its merged collinear LINE (KIWI
    // 2026-09-25: it was the picked edge's midpoint only, and the pick often names a
    // neighbour's shorter collinear edge).
    if ( haveCursorPx )
    {
        bool       have  = false;
        float      bestD = KSNAP_R_EDGE_MID;
        divPt_t    best;
        sel_item_t bestSrc;
        BrushDivisions( ray, curX, curY, KSNAP_R_EDGE_MID, pickFlags,
                        [&]( const divPt_t &d, selbrush_t *b, int f, int i )
        {
            const float dist = PixelDist( d.px, d.py, curX, curY );
            if ( dist > bestD || ( have && dist >= bestD ) || !visible( d.pos ) )
                return;
            have    = true;
            bestD   = dist;
            best    = d;
            bestSrc = Sel_MakeEdge( b, f, i );
        } );
        if ( have )
        {
            out->valid  = true;
            out->type   = best.quarter ? SNAP_EDGE_QUARTER : SNAP_EDGE_MID;
            out->source = bestSrc;
            Copy3( best.pos, out->position );
            Copy3( best.dir, s_edgeDir );    // the marker's along-edge tick
            s_haveEdgeDir = true;
            return true;
        }
    }
    // Cylinder ring centres and axis midpoint are real point targets, including
    // hollow patches with no cap face to hit. Moving selections cannot self-snap.
    if ( haveCursorPx )
    {
        exclSet_t excluded;
        GatherExcluded( pickFlags, excluded );
        float bestPixels = PICK_VERT_PIXELS, bestDepth = 1.0e30f;
        snap_result_t best;
        selbrush_t *lists[] = { &active_brushes, &selected_brushes };
        for ( selbrush_t *head : lists )
        for ( selbrush_t *b = head->next; b && b != head; b = b->next )
        {
            if ( Excluded( excluded, b ) || !CylinderUsable( b ) || !BoundsNearRay( b->def, ray, 10.0f ) ) continue;
            float ends[2][3];
            if ( !KiwiPrim_CylinderAxis( b->def, ends ) ) continue;
            for ( int point = 0; point < 3; ++point )
            {
                float pos[3], x, y, depth = 0;
                for ( int k = 0; k < 3; ++k )
                {
                    pos[k] = point == 2 ? ( ends[0][k]+ends[1][k] ) * 0.5f : ends[point][k];
                    depth += ( pos[k]-ray.origin[k] ) * ray.dir[k];
                }
                if ( depth <= 0 || !Pick_WorldToImage( pos, &x, &y ) ) continue;
                const float pixels = PixelDist( x, y, curX, curY );
                if ( pixels > bestPixels || ( fabsf( pixels-bestPixels ) < 0.01f && depth >= bestDepth ) ) continue;
                // A cylinder's own shell must not hide its centre; unrelated
                // foreground geometry still occludes this target normally.
                if ( !visible.Scene( pos, b, surf.item.brush == b ) ) continue;   // KIWI: own shell skips LOS too
                best.valid = true;
                best.type = SNAP_FACE_CENTER;
                best.source = Sel_MakeObject( b );
                Copy3( pos, best.position );
                bestPixels = pixels; bestDepth = depth;
            }
        }
        if ( best.valid )
        {
            out->valid = true; out->type = best.type; out->source = best.source;
            Copy3( best.position, out->position );
            return true;
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
            float       extDirs[KSNAP_MAX_EXT_AXES * 3] = { 0.0f };
            const char *extNames[KSNAP_MAX_EXT_AXES]    = { 0 };
            int         extCount = 0;
            if ( !planarPlacer )
            {
                // Rank incident directions by cursor collinearity before applying the cap.
                float aim[3];
                Sub3( raw, anchor, aim );
                const bool haveAim = Norm3( aim );
                anchorDir_t inc[KSNAP_MAX_EXT_AXES];
                // The chain's perpendiculars turn in the plane this frame resolved
                // (the surface or ground under the cursor); world up when it has none.
                const float worldUp[3] = { 0.0f, 0.0f, 1.0f };
                extCount = GatherAnchorDirs( anchor, haveAim ? aim : nullptr,
                                             inc, KSNAP_MAX_EXT_AXES,
                                             out->havePlane ? out->planeNormal : worldUp );
                for ( int i = 0; i < extCount; ++i )
                {
                    Copy3( inc[i].dir, extDirs + (size_t)i * 3 );
                    extNames[i] = inc[i].name;
                }
            }

            ScanAxes( Ed_Camera(), anchor, axisBasis, extDirs, extNames, extCount,
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
    const bool conSegVis = conSeg.hit && visible.Surf( conSeg.pos );
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
                const float rawDeg  = atan2f( dv, du ) * (float)KIWI_RAD2DEG;
                float stopDeg = floorf( rawDeg / KCON_ANGLE_STEP + 0.5f )
                              * KCON_ANGLE_STEP;
                float miss = rawDeg - stopDeg;
                if ( miss < 0.0f )
                    miss = -miss;
                // KIWI (2026-09-21): the same 15-degree stops measured from the chain's
                // LAST SEGMENT compete with the world-bearing ones, nearer stop wins -
                // what the planar placers already do (arm 7).  A leg drawn at 37 degrees
                // makes 52 / 67 / 82 ... reachable; 0 / 90 / 180 off it are the "ext" and
                // "perp" guides of the line rank above, which also hold their line.
                bool  relStop = false;
                float relDeg  = 0.0f;
                {
                    float prev[3];
                    if ( KiwiCon_ToolPrevAnchor( prev ) )
                    {
                        float pd[3];
                        Sub3( anchor, prev, pd );
                        const float pu = Dot3( pd, cu ), pv = Dot3( pd, cv );
                        if ( sqrtf( pu * pu + pv * pv ) > 1.0e-3f )
                        {
                            const float baseDeg = atan2f( pv, pu ) * (float)KIWI_RAD2DEG;
                            const float rel     = floorf( ( rawDeg - baseDeg ) / KCON_ANGLE_STEP + 0.5f )
                                                * KCON_ANGLE_STEP;
                            float rmiss = rawDeg - ( baseDeg + rel );
                            if ( rmiss < 0.0f )
                                rmiss = -rmiss;
                            if ( rmiss + 1.0e-3f < miss )
                            {
                                miss    = rmiss;
                                stopDeg = baseDeg + rel;
                                relStop = true;
                                relDeg  = rel;
                                while ( relDeg >   180.0f ) relDeg -= 360.0f;
                                while ( relDeg <= -180.0f ) relDeg += 360.0f;
                            }
                        }
                    }
                }
                // Convert the lateral SCREEN-PIXEL band to degrees at this length, then apply
                // the minimum-degree floor and maximum-step-fraction cap.
                float band = KSNAP_ANGLE_MAX_FRAC * KCON_ANGLE_STEP;
                {
                    const float wpp = WorldPerPixel( Ed_Camera(), raw );
                    if ( wpp > 0.0f )
                    {
                        float deg = ( ( KSNAP_ANGLE_BAND_PIX * wpp ) / len )
                                  * (float)KIWI_RAD2DEG;
                        if ( deg < KSNAP_ANGLE_BAND_MIN )
                            deg = KSNAP_ANGLE_BAND_MIN;
                        if ( deg < band )
                            band = deg;
                    }
                }

                if ( miss <= band )
                {
                    const float rad = stopDeg * (float)KIWI_DEG2RAD;
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
                    s_angleIsRel = relStop;
                    s_angleRel   = relStop ? relDeg : 0.0f;
                    out->valid = true;
                    out->type  = SNAP_ANGLE;
                    return true;
                }
            }
        }
    }
    // Area rank: exact unsnapped surface hit for free tools. Planar shapes suppress
    // this arm because projecting another surface's hit onto their plane misplaces the cursor.
    if ( ( surf.valid || onRegion ) && !planarPlacer )
    {
        out->valid  = true;
        out->type   = SNAP_FACE;
        if ( !onRegion )
            out->source = surf.item;         // a construction face has no brush source
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
                    phi0 = atan2f( Dot3( cu, cplane.v ), Dot3( cu, cplane.u ) ) * (float)KIWI_RAD2DEG;
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
                            baseDeg      = atan2f( rv, ru ) * (float)KIWI_RAD2DEG - phi0;
                            s_angleIsRel = true;
                        }
                    }
                }

                const float rawDeg = atan2f( dv, du ) * (float)KIWI_RAD2DEG - phi0;
                float rel = rawDeg - baseDeg;
                rel = floorf( rel / KCON_ANGLE_STEP + 0.5f ) * KCON_ANGLE_STEP;
                const float deg = baseDeg + rel;                 // canonical degrees
                const float rad = ( deg + phi0 ) * (float)KIWI_DEG2RAD;
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
    if ( r.type == SNAP_VERTEX || r.type == SNAP_EDGE_MID || r.type == SNAP_EDGE_QUARTER
      || r.type == SNAP_ENDPOINT || r.type == SNAP_INTERSECTION
      || r.type == SNAP_FACE_CENTER )
    {
        EmitDotAndRing( c, r.position, wpp );
        // The along-edge tick distinguishes a midpoint from an isolated point.
        if ( ( r.type == SNAP_EDGE_MID || r.type == SNAP_EDGE_QUARTER ) && s_haveEdgeDir )
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
// Advertise the actual point targets: mid/quarter nubs on the edges near the cursor, the
// hovered face's corners and vertex-average center. Requires a compatible live command,
// visible markers, and engaged snapping. Own batches, capped at KSNAP_DIV_DRAW_MAX nubs
// and KSNAP_ACCENT_MAX face points.
void KiwiSnap_DrawFaceAccents()
{
    if ( !KiwiSnap_ShowMarkers() )
        return;
    // Pivot placement and held transform handles may request the same dots.
    // A bounding-box center is omitted because no snap arm can honor it.
    KiwiEditorCommand *cmd = KiwiCmd_Active();
    if ( !cmd || !cmd->ShowsSnap() || ( !cmd->WantsClicks() && !KiwiXform_WantsSnapDots() ) )
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

    // KIWI: black nubs on the midpoints and quarter points of every edge near the cursor
    // (brush and construction), the targets snap rank 3b/4 locks onto.  Independent of the
    // face under the cursor, so a roof edge shows its nubs with the pointer just outside it.
    // Width-2 lines so a 3 px nub reads solid; one nub per spot (edges are shared).
    // KIWI (D-XRAY): advertise only what snapping would take - with x-ray off, scene nubs and
    // model dots the camera cannot see are not drawn (construction nubs always are: their
    // lines draw on top of the scene).
    LosBegin( ray, cmd->PickFlags(), KSNAP_DIV_REACH_PX );
    {
        const float curX = (float)x, curY = (float)y;
        float drawn[KSNAP_DIV_DRAW_MAX][2];
        int   count = 0;
        KiwiLines_Begin( KSNAP_DIV_DRAW_MAX * ( KSNAP_ACCENT_SEGS + KSNAP_ACCENT_SEGS / 2 ), 2 );
        KiwiLines_Color( KSNAP_COL_MARK[0], KSNAP_COL_MARK[1], KSNAP_COL_MARK[2] );
        auto nubOf = [&]( const divPt_t &d, bool scene )
        {
            if ( count >= KSNAP_DIV_DRAW_MAX || d.segPx < KSNAP_NUB_MIN_SEG_PX )
                return;
            for ( int i = 0; i < count; ++i )
                if ( fabsf( drawn[i][0] - d.px ) < 1.5f && fabsf( drawn[i][1] - d.py ) < 1.5f )
                    return;
            if ( scene && !LosVisible( d.pos ) )
                return;
            drawn[count][0] = d.px;
            drawn[count][1] = d.py;
            ++count;
            float p[3];
            Nudge( c, d.pos, p );
            EmitDisc( c, p, ( d.quarter ? KSNAP_QUARTER_NUB_PX : KSNAP_MID_NUB_PX ) * WorldPerPixel( c, d.pos ),
                      KSNAP_ACCENT_SEGS, true );
        };
        auto nub = [&]( const divPt_t &d ) { nubOf( d, false ); };   // construction
        ConDivisions( curX, curY, KSNAP_DIV_REACH_PX, nub );
        BrushDivisions( ray, curX, curY, KSNAP_DIV_REACH_PX, PICKF_NONE,
                        [&]( const divPt_t &d, selbrush_t *, int, int ) { nubOf( d, true ); } );
        // Brush x brush crossings (rank 3) take the midpoint nub's size.
        BrushCrossings( ray, curX, curY, KSNAP_DIV_REACH_PX, PICKF_NONE,
                        [&]( const float *x, float px, float py )
        {
            divPt_t d;
            Copy3( x, d.pos );
            d.px      = px;
            d.py      = py;
            d.segPx   = KSNAP_NUB_MIN_SEG_PX;
            d.quarter = false;
            nubOf( d, true );
        } );
        KiwiLines_Flush();
    }

    // KIWI (2026-10-03, user: "snapping points for each vertex on each xmodel near the
    // cursor"): a dot on every xmodel mesh vertex within the nub reach (rank 2 targets);
    // vertices on a model's far side are dropped by KiwiDrop_ModelVertsNear.
    {
        // A live transform snaps with PICKF_EXCLUDE_SELECTED: its own model vertices are
        // not targets, so they get no dots either.
        kiwiModelVert_t mv[KSNAP_MODEL_DOTS_MAX];
        const int n = KiwiDrop_ModelVertsNear( (float)x, (float)y, KSNAP_DIV_REACH_PX,
                                               ( cmd->PickFlags() & PICKF_EXCLUDE_SELECTED ) != 0,
                                               mv, KSNAP_MODEL_DOTS_MAX );
        if ( n > 0 )
        {
            KiwiLines_Begin( n * ( KSNAP_ACCENT_SEGS + KSNAP_ACCENT_SEGS / 2 ), 1 );
            KiwiLines_Color( KSNAP_COL_MARK[0], KSNAP_COL_MARK[1], KSNAP_COL_MARK[2] );
            for ( int i = 0; i < n; ++i )
            {
                if ( !LosVisible( mv[i].pos ) )          // KIWI (D-XRAY): hidden: no dot
                    continue;
                float p[3];
                Nudge( c, mv[i].pos, p );
                EmitDisc( c, p, KSNAP_ACCENT_PIX * WorldPerPixel( c, mv[i].pos ), KSNAP_ACCENT_SEGS, true );
            }
            KiwiLines_Flush();
        }
    }

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
        // Corners (edge midpoints are the nubs above).
        if ( emitted < KSNAP_ACCENT_MAX )
        {
            float p[3];
            Nudge( c, w->p[i], p );
            EmitDisc( c, p, KSNAP_ACCENT_PIX * WorldPerPixel( c, w->p[i] ),
                      KSNAP_ACCENT_SEGS, true );
            ++emitted;
        }
    }
    // Emit the derived center last so the corners own a tight budget.
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
// Draw only selected cylinders and the hovered target, so dense maps stay legible.
// These are overlays (like transform handles), including the centre inside a shell.
void KiwiSnap_DrawCylinderOverlay( float imgMinX, float imgMinY, float imgW, float imgH )
{
    if ( !KiwiSnap_ShowMarkers() ) return;
    std::vector<selbrush_t *> nodes;
    auto add = [&]( selbrush_t *b )
    {
        if ( !b || nodes.size() >= 32 ) return;
        for ( selbrush_t *existing : nodes ) if ( existing == b ) return;
        nodes.push_back( b );
    };
    // Stop once full: the loops used to run the whole (building-sized) selection every frame.
    for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes && nodes.size() < 32; b = b->next ) add( b );
    const selection_t &selection = KiwiSel();
    for ( size_t i = 0; i < selection.items.size() && nodes.size() < 32; ++i ) add( selection.items[i].brush );
    const ImVec2 mouse = ImGui::GetMousePos();
    // KIWI (2026-10-27, user: "the dx9 is causing the lag, primarily with the UI actions
    // being used ... You need to fix it"): this second whole-map ray every frame was 6% of
    // the main thread in a live sample.  While the hover pass runs it already picked what
    // is under this cursor, so the hovered target is ITS item.  While a command owns the
    // cursor (a drag: 9% of the main thread in a second sample) the target that matters is
    // the snap source, added below, so no ray at all; the overlay picks for itself only
    // when hover is switched off.
    if ( !KiwiCmd_Active() &&
         mouse.x >= imgMinX && mouse.x < imgMinX+imgW && mouse.y >= imgMinY && mouse.y < imgMinY+imgH )
    {
        if ( KiwiUX_ShowHover() )
        {
            const pick_result_t &hover = KiwiHover_Get();
            if ( hover.valid && Sel_BrushLive( hover.item.brush ) )
                add( hover.item.brush );
        }
        else
        {
            ray_t ray;
            if ( Pick_RayFromImagePos( (int)( mouse.x-imgMinX ), (int)( mouse.y-imgMinY ), &ray ) )
                add( Pick( ray, SEL_MASK_OBJECT ).item.brush );
        }
    }
    if ( KiwiCmd_Active() && KiwiCmd_LastSnap().valid && Sel_BrushLive( KiwiCmd_LastSnap().source.brush ) )
        add( KiwiCmd_LastSnap().source.brush );
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->PushClipRect( ImVec2( imgMinX, imgMinY ), ImVec2( imgMinX+imgW, imgMinY+imgH ), true );
    for ( selbrush_t *b : nodes )
    {
        if ( !CylinderUsable( b ) ) continue;
        float ends[2][3];
        if ( !KiwiPrim_CylinderAxis( b->def, ends ) ) continue;
        if ( ends[0][2] > ends[1][2] )
            for ( int k = 0; k < 3; ++k ) { float t = ends[0][k]; ends[0][k] = ends[1][k]; ends[1][k] = t; }
        float centre[3], delta[3], length2 = 0;
        for ( int k = 0; k < 3; ++k )
        {
            centre[k] = ( ends[0][k]+ends[1][k] ) * 0.5f;
            delta[k] = ends[1][k]-ends[0][k]; length2 += delta[k]*delta[k];
        }
        ImVec2 points[3];
        if ( !Pick_WorldToImage( ends[0], &points[0].x, &points[0].y ) ||
             !Pick_WorldToImage( ends[1], &points[1].x, &points[1].y ) ||
             !Pick_WorldToImage( centre, &points[2].x, &points[2].y ) ) continue;
        for ( ImVec2 &v : points ) { v.x += imgMinX; v.y += imgMinY; }
        const ImU32 ink = IM_COL32( 255, 220, 80, 255 ), shadow = IM_COL32( 20, 20, 24, 230 );
        draw->AddLine( points[0], points[1], shadow, 4 );
        draw->AddLine( points[0], points[1], ink, 2 );
        for ( int i = 0; i < 3; ++i )
        {
            const ImVec2 v = points[i];
            draw->AddCircleFilled( v, 6, shadow, 16 );
            draw->AddCircle( v, 5, ink, 16, 1.5f );
            draw->AddLine( ImVec2(v.x-8,v.y), ImVec2(v.x+8,v.y), ink );
            draw->AddLine( ImVec2(v.x,v.y-8), ImVec2(v.x,v.y+8), ink );
        }
        const float length = sqrtf( length2 );
        float z = fabsf( delta[2] ) / length;
        if ( z > 1 ) z = 1;
        const float tilt = acosf( z ) * (float)KIWI_RAD2DEG;
        // A dotted world-up reference makes actual tilt visible independently
        // of perspective. It is intentionally not another snapping axis.
        if ( tilt > 0.05f )
        {
            float up[3] = { ends[0][0], ends[0][1], ends[0][2]+length };
            float x,y;
            if ( Pick_WorldToImage( up, &x, &y ) )
            {
                ImVec2 end( x+imgMinX, y+imgMinY );
                const ImVec2 start = points[0];
                for ( int j = 0; j < 16; j += 2 )
                {
                    float t0 = j/16.0f, t1 = (j+1)/16.0f;
                    draw->AddLine( ImVec2(start.x+(end.x-start.x)*t0,start.y+(end.y-start.y)*t0),
                                   ImVec2(start.x+(end.x-start.x)*t1,start.y+(end.y-start.y)*t1),
                                   IM_COL32(100,210,255,220), 1.5f );
                }
                draw->AddText( ImVec2(end.x+5,end.y), IM_COL32(100,210,255,255), "Z up" );
            }
        }
        char label[96];
        if ( tilt <= 0.05f ) snprintf( label, sizeof(label), "Axis: vertical (90 deg to XY)" );
        else snprintf( label, sizeof(label), "Axis: %.1f deg from vertical", tilt );
        const ImVec2 pos( points[2].x+12, points[2].y+9 ), size = ImGui::CalcTextSize( label );
        draw->AddRectFilled( ImVec2(pos.x-3,pos.y-2), ImVec2(pos.x+size.x+3,pos.y+size.y+2), shadow, 3 );
        draw->AddText( pos, ink, label );
    }
    draw->PopClipRect();
}

// Screen-space label drawn during the ImGui frame.
void KiwiSnap_DrawLabel( const snap_result_t &r, float imgMinX, float imgMinY,
                         float imgW, float imgH )
{
    if ( !r.valid || !KiwiSnap_ShowMarkers() )
        return;

    // KIWI (Plasticity 2026.2): an xmodel vertex snap shows the mesh triangle it came from as
    // a light partial wireframe (s_meshTri, KiwiSnap_Query).  Brush vertices carry a source.
    if ( r.type == SNAP_VERTEX && !r.source.brush && s_meshTriValid
      && r.position[0] == s_meshTriAt[0] && r.position[1] == s_meshTriAt[1]
      && r.position[2] == s_meshTriAt[2] )
    {
        ImVec2 p[3];
        bool ok = true;
        for ( int c = 0; c < 3 && ok; ++c )
        {
            ok = Pick_WorldToImage( s_meshTri[c], &p[c].x, &p[c].y );
            p[c].x += imgMinX;
            p[c].y += imgMinY;
        }
        if ( ok )
        {
            ImDrawList *tl = ImGui::GetWindowDrawList();
            tl->PushClipRect( ImVec2( imgMinX, imgMinY ), ImVec2( imgMinX + imgW, imgMinY + imgH ), true );
            tl->AddTriangleFilled( p[0], p[1], p[2], IM_COL32( 120, 255, 145, 28 ) );
            tl->AddTriangle( p[0], p[1], p[2], IM_COL32( 190, 255, 205, 170 ), 1.0f );
            tl->PopClipRect();
        }
    }

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
    if ( r.type == SNAP_VERTEX || r.type == SNAP_EDGE_MID || r.type == SNAP_EDGE_QUARTER
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

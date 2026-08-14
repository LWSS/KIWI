#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_construct.cpp — RADIANT_UX_DESIGN §7 / §16 / Phase-4 items 21+22.
// See kiwi_construct.h for the four scope rulings this file is built on
// (construction geometry is outside selection_t, it has its own undo stack, its
// points live in plane space, and its persistence is a sidecar).
//
// NEW code over the ported cores.  Nothing here mutates map data; the only
// ported entry points it touches are the camera accessors, the pick API and the
// line batcher.  Brush creation lives in kiwi_extrude.cpp.
//
// ── THE SIDECAR FORMAT (KIWI2 — shakeout H) ─────────────────────────────────
// Line based, whitespace separated, first token is the keyword.  Blank lines and
// '#' comments are skipped.  Sample (a rect and a circle on the ground plane):
//
//     KIWI2
//     # KIWI construction geometry — do not hand-edit while Radiant is open
//     object rect
//     closed 1
//     wpt 0 0 0
//     wpt 128 0 0
//     wpt 128 96 0
//     wpt 0 96 0
//     end
//     object circle
//     plane 0 0 0  0 0 1  1 0 0  0 1 0
//     closed 1
//     arc 256 128 64 0 360
//     end
//
//   KIWI2                       the version line
//   object <line|polyline|rect|circle|arc>
//   plane  ox oy oz  nx ny nz  ux uy uz  vx vy vz   (CIRCLE / ARC only, now)
//   closed 0|1
//   wpt    x y z                (LINE / POLYLINE / RECT — WORLD space)
//   arc    cu cv radius ang0 ang1   (CIRCLE / ARC, degrees)
//   end
//
// ── KIWI1 STILL LOADS ───────────────────────────────────────────────────────
// The v1 format wrote `plane` for EVERY object and `pt u v` in that plane's basis
// (ruling 3 as it stood before shakeout H).  Both keywords are still parsed: a
// `pt` is stashed in plane space and converted to world through the object's
// `plane` at its `end`.  The reader therefore accepts KIWI1 and KIWI2 files (and
// even a hand-mixed one); the writer only ever emits KIWI2.
//
// An unknown keyword inside an object is skipped; an unknown object TYPE skips to
// its `end`.  A malformed file is reported once on the console and dropped —
// never a crash, never a blocked map load (§7).
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"        // camera_s
#include "prefs.h"          // g_PrefsDlg (camera_fov — the screen-scaled point marker)

#include <imgui/imgui.h>

#include "kiwi_camera.h"             // ROUND M: KiwiCam_WorldPerPixel (ortho-aware screen-scale)
#include "kiwi_construct.h"
#include "kiwi_command.h"
#include "kiwi_conselect.h"          // shakeout F — selected construction geometry
#include "kiwi_fillet.h"             // round J — the Construct block's Fillet button
#include "kiwi_grid.h"
#include "kiwi_hover.h"              // ROUND AG, ITEM 3 - the outliner row hover
#include "kiwi_lines.h"
#include "kiwi_offset.h"             // round J — …and its Offset button
#include "kiwi_numeric.h"
#include "kiwi_pick.h"
#include "kiwi_region.h"
#include "kiwi_snap.h"
#include "kiwi_trim.h"               // shakeout H — the Construct block's Trim button
#include "kiwi_undo.h"               // shakeout I — the unified undo journal
#include "kiwi_units.h"
#include "kiwi_viewcube.h"            // ROUND Y: KiwiViewCube_ViewAxis (the axis-view rung)
#include "kiwi_visibility.h"          // ROUND AO: the §7 sidecar also carries hidden SOLIDS
#include "radiant_registry.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ── ported entry points (each verified against its definition) ──────────────
extern int       Sys_Printf( const char *fmt, ... );          // win_qe3.cpp
extern camera_s *Ed_Camera();                                 // camwnd.cpp
extern void      CamWnd_BuildMatrix();                        // camwnd.cpp 0x403470
extern int       g_nUpdateBits;                               // 0x25D5A74 (mainfrm.cpp)
extern bool      ImGuiShell_CameraPaintCursor( int *x, int *y, int *w, int *h );  // imgui_shell.cpp
extern bool      Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId );  // mainfrm.cpp

namespace
{
    const char *KCON_SECTION = "KiwiUX";
    int         s_showConstruction = -1;          // -1 = not loaded yet

    // §18's language: construction geometry gets a colour NOTHING else uses —
    // not hover cyan (0.35,0.95,1.00), not the active amber (1.00,0.80,0.25),
    // not the red/green/blue axes, not the white selection outline and not the
    // magenta selected-face wireframe.  Rose reads as "editor only" at a glance.
    const float KCON_COL_LINE  [3] = { 1.00f, 0.35f, 0.70f };
    const float KCON_COL_POINT [3] = { 1.00f, 0.62f, 0.85f };
    const float KCON_COL_ACTIVE[3] = { 1.00f, 0.85f, 0.45f };   // the in-progress chain
    // SHAKEOUT F: the SELECTED construction colours — the rose base, pushed most
    // of the way to white.  See the note at the draw site for why white and not a
    // new hue.
    const float KCON_COL_SEL_LINE [3] = { 1.00f, 0.88f, 0.94f };
    const float KCON_COL_SEL_POINT[3] = { 1.00f, 1.00f, 1.00f };
    // ROUND AG, ITEM 3: the OUTLINER-hovered curve.  Section 18's hover hue,
    // shared with kiwi_hover.cpp's KHOVER_COL, so a hovered row means the same
    // colour whether the thing it names is a brush or a curve.
    const float KCON_COL_HOVER[3] = { 0.35f, 0.95f, 1.00f };
    // (ROUND M: KCON_COL_PLANE / KCON_COL_PLANEX / KCON_PATCH_CELLS lived here.
    //  They belonged to the construction-plane grid patch, which is deleted — the
    //  directive and the "what carries the feedback now" argument are in the block
    //  just above KiwiCon_DrawWorld.  Shakeout H's grey-instead-of-pink pass and
    //  its ground-plane suppression went with it: two rounds of making the patch
    //  quieter did not make it wanted.)

    // ── the store ───────────────────────────────────────────────────────────
    std::vector<kconObject_t>                s_objects;
    std::vector< std::vector<kconObject_t> > s_undo;
    // SHAKEOUT I: the redo half.  Ruling 2 shipped an undo with no redo at all,
    // which is half a feature; the unified journal (kiwi_undo.h) drives both.
    std::vector< std::vector<kconObject_t> > s_redo;
    unsigned                                 s_generation = 1;

    // ── ROUND W: the construction GROUP table (kiwi_construct.h) ────────────
    // A plain vector of {id, name} — the counts are tens, not thousands.  Ids are
    // HANDLES minted from a monotonic counter, never indices, so removing a group
    // cannot silently rename another one in a sidecar somebody already saved.
    // The names are a fixed buffer rather than std::string for the same reason the
    // rest of this file uses char[]: no new include, and 63 characters is longer
    // than any folder name a mapper will type twice.
    const int KCON_GROUPNAME_MAX = 64;
    struct kconGroup_t
    {
        int  id;
        char name[KCON_GROUPNAME_MAX];
    };
    std::vector<kconGroup_t> s_groups;
    int                      s_nextGroupId = 1;

    int GroupSlot( int id )
    {
        if ( id < 0 )
            return -1;
        for ( size_t i = 0; i < s_groups.size(); ++i )
            if ( s_groups[i].id == id )
                return (int)i;
        return -1;
    }

    kconPlane_t s_plane;                          // the ACTIVE construction plane
    bool        s_planeSeeded = false;

    // ── small vector helpers (same spelling as kiwi_transform.cpp) ──────────
    inline float Dot3( const float *a, const float *b )
    {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }
    inline void Copy3( const float *a, float *o ) { o[0]=a[0]; o[1]=a[1]; o[2]=a[2]; }
    inline void Sub3( const float *a, const float *b, float *o )
    { o[0]=a[0]-b[0]; o[1]=a[1]-b[1]; o[2]=a[2]-b[2]; }
    inline void Mad3( const float *a, const float *d, float s, float *o )
    { o[0]=a[0]+d[0]*s; o[1]=a[1]+d[1]*s; o[2]=a[2]+d[2]*s; }
    inline void Cross3( const float *a, const float *b, float *o )
    {
        o[0] = a[1]*b[2] - a[2]*b[1];
        o[1] = a[2]*b[0] - a[0]*b[2];
        o[2] = a[0]*b[1] - a[1]*b[0];
    }
    inline float Len3( const float *a ) { return sqrtf( Dot3( a, a ) ); }
    bool Norm3( float *v )
    {
        const float l = Len3( v );
        if ( !( l > 1.0e-6f ) )
            return false;
        v[0] /= l; v[1] /= l; v[2] /= l;
        return true;
    }

    void Touch()
    {
        ++s_generation;
        KiwiRegion_Invalidate();
        g_nUpdateBits |= 1;
    }

    // ═════════════════════════════════════════════════════════════════════════
    //  ROUND AG, ITEM 5(b) — CARDINAL ANCHORING
    // ═════════════════════════════════════════════════════════════════════════
    // USER DIRECTIVE, verbatim: "When making a circle, there need to be segments
    // on each 90 degree point so I can make more advanced shapes like ovals."
    //
    // KiwiCon_VertWorld already phases the ring at ang = 0 — vertex 0 sits exactly
    // on the plane's +U axis — so the 0-degree cardinal has always been hit.  The
    // other three are hit if and ONLY IF the segment count is a MULTIPLE OF FOUR,
    // because vertex k is at 2*pi*k/n and 90/180/270 degrees are k = n/4, n/2,
    // 3n/4.  The automatic rule (radius * 0.25) produces 8, 9, 10, 11 … and three
    // counts in four therefore miss all three.
    //
    // WHY IT MATTERS, and it is not cosmetic.  Scale a ring anisotropically and
    // you get an ellipse; the SHAPE of that ellipse is right whatever the phase,
    // but its EXTREMES — the four points a mapper actually aligns to, snaps to and
    // extrudes from — only exist as vertices when the cardinals are vertices.  Off
    // phase, the ellipse's major axis ends in a flat chord instead of a point, the
    // bounding box is smaller than the nominal radii, and the quadrant symmetry
    // the user is asking for is simply absent.
    //
    // So the count ROUNDS UP to the next multiple of four.  UP rather than to
    // nearest: a count is a smoothness request and rounding it down would silently
    // give less than was asked for.  The ceiling KCON_SEGS_MAX (64) is already a
    // multiple of four, so the clamp costs nothing at the top.
    int CardinalSegs( int n )
    {
        if ( n < 4 )
            n = 4;
        n = ( ( n + 3 ) / 4 ) * 4;
        if ( n > KCON_SEGS_MAX )
            n = ( KCON_SEGS_MAX / 4 ) * 4;
        return n;
    }

    // How many segments a full circle of this radius gets (kiwi_construct.h).
    int CircleSegs( float radius )
    {
        int n = (int)floorf( fabsf( radius ) * KCON_SEGS_PER_UNIT + 0.5f );
        if ( n < KCON_SEGS_MIN ) n = KCON_SEGS_MIN;
        if ( n > KCON_SEGS_MAX ) n = KCON_SEGS_MAX;
        return CardinalSegs( n );      // ROUND AG, ITEM 5(b)
    }

    // ── KIWI-UX (ROUND AF, ITEM 7): THE USER'S COUNT OUTRANKS THE RULE ──────
    // A full circle's segment count: the object's own `segs` when it has one, else
    // the radius-driven rule.  ONE choke point, so every reader — VertCount,
    // VertWorld, SegmentCount, the region derivation, the extrude profile and the
    // draw pass — agrees without any of them knowing the override exists.
    int CircleSegsFor( const kconObject_t &o )
    {
        if ( o.segs <= 0 )
            return CircleSegs( o.radius );
        int n = o.segs;
        if ( n < KCON_SIDES_MIN ) n = KCON_SIDES_MIN;
        if ( n > KCON_SEGS_MAX  ) n = KCON_SEGS_MAX;
        // ROUND AG, ITEM 5(b): a TYPED count rounds up to a multiple of four for
        // exactly the same reason the automatic one does — the user asked for the
        // cardinals, not for their count to be honoured to the unit.  The HUD and
        // the Tab field both report the rounded number (KiwiCon_CardinalSides), so
        // typing 30 and seeing 32 is a statement the editor makes out loud rather
        // than a discrepancy the mapper has to discover from the geometry.
        //
        // The ARC deliberately does NOT get this: ArcSegs takes this count PRO RATA
        // over its own sweep, which is arbitrary, so there are no cardinals to hit
        // and forcing a multiple of four there would only coarsen it.
        return CardinalSegs( n );
    }

    // An arc gets the same density pro-rata over its own sweep, never below 2.
    int ArcSegs( const kconObject_t &o )
    {
        const float sweep = fabsf( o.ang1 - o.ang0 );
        int n = (int)floorf( (float)CircleSegsFor( o ) * ( sweep / 360.0f ) + 0.5f );
        if ( n < 2 ) n = 2;
        if ( n > KCON_SEGS_MAX ) n = KCON_SEGS_MAX;
        return n;
    }

    void ArcPointUV( const kconObject_t &o, float t, float outUV[2] )
    {
        const float deg = o.ang0 + ( o.ang1 - o.ang0 ) * t;
        const float rad = deg * 0.01745329252f;
        outUV[0] = o.centre[0] + cosf( rad ) * o.radius;
        outUV[1] = o.centre[1] + sinf( rad ) * o.radius;
    }

    // ── §16b spline tessellation (the ONE spelling of it) ───────────────────
    // Uniform Catmull-Rom through the control points, KCON_SPLINE_SEGS segments
    // per span, mirroring the circle/arc rule: the store holds a TESSELLATION,
    // never a curve, so region detection and extrusion keep working on the same
    // straight-segment polyline they already understand.
    //
    // End handling: an OPEN spline reflects its end tangents (P[-1] := P[0],
    // P[n] := P[n-1]), which makes the curve start and finish exactly on the
    // first and last clicked point.  A CLOSED spline wraps instead.
    //
    // SHAKEOUT H: 3 components, not 2 — ruling 3 sends the spline free into world
    // space with the line and the polyline.  Catmull-Rom is per-component, so this
    // is literally the same blend run one more time.
    void SplinePoint3( const float *p0, const float *p1, const float *p2,
                       const float *p3, float t, float out[3] )
    {
        const float t2 = t * t;
        const float t3 = t2 * t;
        for ( int k = 0; k < 3; ++k )
        {
            out[k] = 0.5f * ( ( 2.0f * p1[k] )
                            + ( -p0[k] + p2[k] ) * t
                            + ( 2.0f * p0[k] - 5.0f * p1[k] + 4.0f * p2[k] - p3[k] ) * t2
                            + ( -p0[k] + 3.0f * p1[k] - 3.0f * p2[k] + p3[k] ) * t3 );
        }
    }

    // `ctrl` is 3 floats per control point, WORLD space.  Emits at most
    // KCON_MAX_POINTS points (the store's own cap — hitting it coarsens the
    // density rather than truncating the shape, so the curve stays whole).
    void TessellateSpline( const std::vector<float> &ctrl, bool closed,
                           std::vector<float> *out )
    {
        out->clear();
        const int n = (int)( ctrl.size() / 3 );
        if ( n < 2 )
            return;
        if ( n == 2 && !closed )                  // two points is a line, not a curve
        {
            out->assign( ctrl.begin(), ctrl.end() );
            return;
        }

        const int spans = closed ? n : ( n - 1 );
        int segs = KCON_SPLINE_SEGS;
        while ( segs > 1 && spans * segs + 1 > KCON_MAX_POINTS )
            --segs;

        for ( int s = 0; s < spans; ++s )
        {
            const int i0 = closed ? ( ( s - 1 + n ) % n ) : ( ( s - 1 < 0 ) ? 0 : s - 1 );
            const int i1 = s;
            const int i2 = closed ? ( ( s + 1 ) % n ) : ( ( s + 1 > n - 1 ) ? n - 1 : s + 1 );
            const int i3 = closed ? ( ( s + 2 ) % n ) : ( ( s + 2 > n - 1 ) ? n - 1 : s + 2 );
            for ( int k = 0; k < segs; ++k )
            {
                float w[3];
                SplinePoint3( &ctrl[(size_t)i0 * 3], &ctrl[(size_t)i1 * 3],
                              &ctrl[(size_t)i2 * 3], &ctrl[(size_t)i3 * 3],
                              (float)k / (float)segs, w );
                out->push_back( w[0] );
                out->push_back( w[1] );
                out->push_back( w[2] );
            }
        }
        if ( !closed )                            // land exactly on the last click
        {
            out->push_back( ctrl[( (size_t)n - 1 ) * 3 + 0] );
            out->push_back( ctrl[( (size_t)n - 1 ) * 3 + 1] );
            out->push_back( ctrl[( (size_t)n - 1 ) * 3 + 2] );
        }
    }
}

// ─── plane math ──────────────────────────────────────────────────────────────
void KiwiCon_PlaneToWorld( const kconPlane_t &p, const float uv[2], float out[3] )
{
    for ( int k = 0; k < 3; ++k )
        out[k] = p.origin[k] + p.u[k] * uv[0] + p.v[k] * uv[1];
}

void KiwiCon_WorldToPlane( const kconPlane_t &p, const float world[3], float outUV[2] )
{
    float rel[3];
    Sub3( world, p.origin, rel );
    outUV[0] = Dot3( rel, p.u );
    outUV[1] = Dot3( rel, p.v );
}

bool KiwiCon_RayPlane( const kconPlane_t &p, const ray_t &ray, float outWorld[3] )
{
    const float den = Dot3( ray.dir, p.normal );
    if ( fabsf( den ) < 1.0e-5f )
        return false;
    float rel[3];
    Sub3( p.origin, ray.origin, rel );
    const float t = Dot3( rel, p.normal ) / den;
    if ( !( t > 0.0f ) || t > 1.0e6f )
        return false;
    Mad3( ray.origin, ray.dir, t, outWorld );
    return true;
}

// ── ROUND X, ITEM 2: the FINITE plane.  Full rationale + the Plasticity cites
// are on the declaration in kiwi_construct.h.
bool KiwiCon_RayPlaneBounded( const kconPlane_t &p, const ray_t &ray, float outWorld[3] )
{
    // A degenerate plane basis has no (u,v) to bound anything in.
    if ( !( Dot3( p.normal, p.normal ) > 0.5f ) )
        return false;

    float hit[3];
    bool  have = false;

    const float den = Dot3( ray.dir, p.normal );
    if ( fabsf( den ) >= 1.0e-5f )
    {
        float rel[3];
        Sub3( p.origin, ray.origin, rel );
        const float t = Dot3( rel, p.normal ) / den;
        if ( t > 0.0f )
        {
            Mad3( ray.origin, ray.dir, t, hit );
            have = true;
        }
    }

    if ( !have )
    {
        // No forward intersection — the ray is parallel to the plane, or the plane
        // is behind the eye.  Start at the ray origin dropped onto the plane and run
        // out along the ray's IN-PLANE bearing; the clamp below stops it at the quad
        // edge.  With no in-plane bearing at all (the ray is the plane normal, which
        // cannot also be parallel to it) the origin projection is the answer.
        float rel[3];
        Sub3( ray.origin, p.origin, rel );
        const float off = Dot3( rel, p.normal );
        for ( int k = 0; k < 3; ++k )
            hit[k] = ray.origin[k] - p.normal[k] * off;

        float d[3];
        const float dn = Dot3( ray.dir, p.normal );
        for ( int k = 0; k < 3; ++k )
            d[k] = ray.dir[k] - p.normal[k] * dn;
        if ( Norm3( d ) )
            for ( int k = 0; k < 3; ++k )
                hit[k] += d[k] * ( KCON_PLANE_REACH * 2.0f );   // past the edge; clamped below
    }

    // THE QUAD.  Clamp in the plane's own basis, so the bound is a square centred on
    // the plane origin — Plasticity's PlaneGeometry(10000, 10000), in plane space.
    float uv[2];
    KiwiCon_WorldToPlane( p, hit, uv );
    for ( int k = 0; k < 2; ++k )
    {
        if ( uv[k] >  KCON_PLANE_REACH ) uv[k] =  KCON_PLANE_REACH;
        if ( uv[k] < -KCON_PLANE_REACH ) uv[k] = -KCON_PLANE_REACH;
        if ( !( uv[k] == uv[k] ) )       uv[k] = 0.0f;          // NaN from a huge t
    }
    KiwiCon_PlaneToWorld( p, uv, outWorld );
    return true;
}

bool KiwiCon_MakePlane( const float origin[3], const float normal[3],
                        const float *hintU, kconPlane_t *out )
{
    if ( !out )
        return false;
    float n[3];
    Copy3( normal, n );
    if ( !Norm3( n ) )
        return false;

    // Seed u from the hint when it is usable, else from the world axis LEAST
    // aligned with the normal — the standard trick, and the one that keeps an
    // axis-aligned plane's u/v on world axes (so its grid looks like the world's).
    float u[3];
    bool haveU = false;
    if ( hintU )
    {
        Copy3( hintU, u );
        Mad3( u, n, -Dot3( u, n ), u );          // strip the out-of-plane part
        haveU = Norm3( u );
    }
    if ( !haveU )
    {
        int least = 0;
        for ( int k = 1; k < 3; ++k )
            if ( fabsf( n[k] ) < fabsf( n[least] ) )
                least = k;
        float ax[3] = { 0.0f, 0.0f, 0.0f };
        ax[least] = 1.0f;
        Cross3( n, ax, u );
        if ( !Norm3( u ) )
            return false;
    }

    float v[3];
    Cross3( n, u, v );                            // right-handed: u × v == n
    if ( !Norm3( v ) )
        return false;

    Copy3( origin, out->origin );
    Copy3( n, out->normal );
    Copy3( u, out->u );
    Copy3( v, out->v );
    return true;
}

// KIWI-UX (ROUND R): the lattice is anchored at the WORLD ORIGIN's projection onto
// the plane, not at the plane's own (arbitrary, tool-written) origin.  The whole
// root-cause chain is written out on the declaration in kiwi_construct.h.
void KiwiCon_SnapUV( const kconPlane_t &p, float uv[2] )
{
    const float s = KiwiUnits_GridSpacingWorld();
    if ( !( s > 0.0f ) )
        return;
    // The plane origin's OWN in-plane coordinates, measured from the world origin.
    // Adding them makes `uv` absolute, snapping is then a world-lattice question,
    // and subtracting them puts the answer back in the caller's basis.  For an
    // axis-aligned plane u and v are world axes, so this IS "snap x and y to the
    // world grid" — no special case, and the normal component never moves.
    const float ou = Dot3( p.origin, p.u );
    const float ov = Dot3( p.origin, p.v );
    uv[0] = floorf( ( uv[0] + ou ) / s + 0.5f ) * s - ou;
    uv[1] = floorf( ( uv[1] + ov ) / s + 0.5f ) * s - ov;
}

// ─── SHAKEOUT H: the DERIVED plane (ruling 3) ────────────────────────────────
bool KiwiCon_FitPlane( const float *worldPts, int count, kconPlane_t *out )
{
    if ( !worldPts || count < 3 || !out )
        return false;

    // NEWELL'S METHOD.  The sum of the cross products of consecutive edge pairs,
    // written in its area form so it is exact for a planar polygon and stable for
    // a nearly-degenerate one.  A plain (p1-p0)x(p2-p0) would collapse whenever
    // the FIRST THREE points happen to be collinear — which is routine here,
    // because a rect's first three points are two collinear edges and a chain
    // welded from straight lines starts with a straight run.
    float n[3] = { 0.0f, 0.0f, 0.0f };
    float c[3] = { 0.0f, 0.0f, 0.0f };
    for ( int i = 0; i < count; ++i )
    {
        const float *a = &worldPts[(size_t)i * 3];
        const float *b = &worldPts[(size_t)( ( i + 1 ) % count ) * 3];
        n[0] += ( a[1] - b[1] ) * ( a[2] + b[2] );
        n[1] += ( a[2] - b[2] ) * ( a[0] + b[0] );
        n[2] += ( a[0] - b[0] ) * ( a[1] + b[1] );
        c[0] += a[0];  c[1] += a[1];  c[2] += a[2];
    }
    if ( !Norm3( n ) )
        return false;                             // collinear / zero area
    for ( int k = 0; k < 3; ++k )
        c[k] /= (float)count;

    // Seed u from the LONGEST edge, the same choice PlaneFromFacePick makes for a
    // brush face: it is the numerically safest in-plane direction available and it
    // lines the object's own grid up with the object.
    // SHAKEOUT H FIX: the scan WRAPS, exactly as the Newell sum above does.  It
    // used to stop at count-1 and skip the closing edge, so a loop whose longest
    // edge happened to be the closing one seeded u from a shorter edge instead —
    // harmless (any in-plane direction is a legal basis) but gratuitously different
    // from the normal it is paired with.
    float longest[3] = { 0.0f, 0.0f, 0.0f };
    float bestLen    = 0.0f;
    for ( int i = 0; i < count; ++i )
    {
        float e[3];
        Sub3( &worldPts[(size_t)( ( i + 1 ) % count ) * 3], &worldPts[(size_t)i * 3], e );
        const float l = Len3( e );
        if ( l > bestLen ) { bestLen = l; Copy3( e, longest ); }
    }

    kconPlane_t p;
    if ( !KiwiCon_MakePlane( c, n, ( bestLen > 1.0e-3f ) ? longest : 0, &p ) )
        return false;

    // THE GATE.  Every point must sit within KCON_PLANE_FIT_DIST of the fit, or
    // this is not a planar object and saying it is would be a lie the region and
    // extrude paths would then act on.
    for ( int i = 0; i < count; ++i )
    {
        float rel[3];
        Sub3( &worldPts[(size_t)i * 3], p.origin, rel );
        if ( fabsf( Dot3( rel, p.normal ) ) > KCON_PLANE_FIT_DIST )
            return false;
    }
    *out = p;
    return true;
}

bool KiwiCon_ObjectPlane( const kconObject_t &o, kconPlane_t *out )
{
    if ( !out )
        return false;
    if ( KiwiCon_IsParametric( o ) || o.planeValid )
    {
        *out = o.plane;
        return true;
    }
    return false;
}

float KiwiCon_SegSegClosest( const float a0[3], const float a1[3],
                             const float b0[3], const float b1[3],
                             float *outTa, float *outTb, float outMid[3] )
{
    // The textbook clamped closest-approach solve (Ericson, Real-Time Collision
    // Detection §5.1.9), written out rather than cited-and-approximated because
    // the trim tool's whole correctness rests on it.
    float d1[3], d2[3], r[3];
    Sub3( a1, a0, d1 );
    Sub3( b1, b0, d2 );
    Sub3( a0, b0, r );
    const float aa = Dot3( d1, d1 );
    const float e  = Dot3( d2, d2 );
    const float f  = Dot3( d2, r );

    float s = 0.0f, t = 0.0f;
    if ( aa <= 1.0e-8f && e <= 1.0e-8f )
    {
        s = t = 0.0f;                             // both degenerate: point vs point
    }
    else if ( aa <= 1.0e-8f )
    {
        t = f / e;
        if ( t < 0.0f ) t = 0.0f;  if ( t > 1.0f ) t = 1.0f;
    }
    else
    {
        const float cc = Dot3( d1, r );
        if ( e <= 1.0e-8f )
        {
            s = -cc / aa;
            if ( s < 0.0f ) s = 0.0f;  if ( s > 1.0f ) s = 1.0f;
        }
        else
        {
            const float b    = Dot3( d1, d2 );
            const float den  = aa * e - b * b;
            if ( den > 1.0e-8f )
            {
                s = ( b * f - cc * e ) / den;
                if ( s < 0.0f ) s = 0.0f;  if ( s > 1.0f ) s = 1.0f;
            }
            t = ( b * s + f ) / e;
            if ( t < 0.0f )
            {
                t = 0.0f;
                s = -cc / aa;
                if ( s < 0.0f ) s = 0.0f;  if ( s > 1.0f ) s = 1.0f;
            }
            else if ( t > 1.0f )
            {
                t = 1.0f;
                s = ( b - cc ) / aa;
                if ( s < 0.0f ) s = 0.0f;  if ( s > 1.0f ) s = 1.0f;
            }
        }
    }

    float pa[3], pb[3], diff[3];
    Mad3( a0, d1, s, pa );
    Mad3( b0, d2, t, pb );
    Sub3( pa, pb, diff );
    if ( outTa ) *outTa = s;
    if ( outTb ) *outTb = t;
    if ( outMid )
        for ( int k = 0; k < 3; ++k )
            outMid[k] = ( pa[k] + pb[k] ) * 0.5f;
    return Len3( diff );
}

// ─── tessellated geometry ────────────────────────────────────────────────────
int KiwiCon_VertCount( const kconObject_t &o )
{
    if ( o.type == KCON_CIRCLE )
        return CircleSegsFor( o );                // closed: the wrap is implicit
    if ( o.type == KCON_ARC )
        return ArcSegs( o ) + 1;
    return (int)( o.pts.size() / 3 );             // shakeout H: 3 floats, WORLD
}

bool KiwiCon_VertWorld( const kconObject_t &o, int i, float out[3] )
{
    const int n = KiwiCon_VertCount( o );
    if ( i < 0 || i >= n )
        return false;
    if ( o.type == KCON_CIRCLE )
    {
        // A full ring regardless of ang0/ang1 — a circle IS 360°, and storing the
        // pair keeps one parametric block for both types.
        const float rad = ( (float)i / (float)n ) * 6.283185307f;
        const float uv[2] = { o.centre[0] + cosf( rad ) * o.radius,
                              o.centre[1] + sinf( rad ) * o.radius };
        KiwiCon_PlaneToWorld( o.plane, uv, out );
        return true;
    }
    if ( o.type == KCON_ARC )
    {
        float uv[2];
        ArcPointUV( o, (float)i / (float)( n - 1 ), uv );
        KiwiCon_PlaneToWorld( o.plane, uv, out );
        return true;
    }
    Copy3( &o.pts[(size_t)i * 3], out );          // world, verbatim
    return true;
}

int KiwiCon_SegmentCount( const kconObject_t &o )
{
    const int n = KiwiCon_VertCount( o );
    if ( n < 2 )
        return 0;
    const bool closed = ( o.type == KCON_CIRCLE ) || ( o.type == KCON_RECT ) || o.closed;
    return closed ? n : ( n - 1 );
}

bool KiwiCon_SegmentWorld( const kconObject_t &o, int i, float a[3], float b[3] )
{
    const int segs = KiwiCon_SegmentCount( o );
    if ( i < 0 || i >= segs )
        return false;
    const int n = KiwiCon_VertCount( o );
    return KiwiCon_VertWorld( o, i, a ) && KiwiCon_VertWorld( o, ( i + 1 ) % n, b );
}

int KiwiCon_AnchorCount( const kconObject_t &o )
{
    if ( o.type == KCON_CIRCLE ) return 5;        // centre + 4 quadrants
    if ( o.type == KCON_ARC )    return 3;        // centre + both ends
    return (int)( o.pts.size() / 3 );
}

bool KiwiCon_AnchorWorld( const kconObject_t &o, int i, float out[3] )
{
    if ( i < 0 || i >= KiwiCon_AnchorCount( o ) )
        return false;

    float uv[2];
    if ( o.type == KCON_CIRCLE )
    {
        if ( i == 0 ) { uv[0] = o.centre[0]; uv[1] = o.centre[1]; }
        else
        {
            const float rad = (float)( i - 1 ) * 1.570796327f;
            uv[0] = o.centre[0] + cosf( rad ) * o.radius;
            uv[1] = o.centre[1] + sinf( rad ) * o.radius;
        }
    }
    else if ( o.type == KCON_ARC )
    {
        if ( i == 0 ) { uv[0] = o.centre[0]; uv[1] = o.centre[1]; }
        else            ArcPointUV( o, ( i == 1 ) ? 0.0f : 1.0f, uv );
    }
    else
    {
        Copy3( &o.pts[(size_t)i * 3], out );      // shakeout H: already world
        return true;
    }
    KiwiCon_PlaneToWorld( o.plane, uv, out );
    return true;
}

// ─── the store ───────────────────────────────────────────────────────────────
int KiwiCon_Count()
{
    return (int)s_objects.size();
}

const kconObject_t *KiwiCon_At( int index )
{
    if ( index < 0 || index >= (int)s_objects.size() )
        return 0;
    return &s_objects[index];
}

namespace
{
    // SHAKEOUT H: re-derive one point object's cached plane (kiwi_construct.h).
    // Parametric objects are left alone — their plane is authoritative.
    void RefitPlane( kconObject_t &o )
    {
        if ( KiwiCon_IsParametric( o ) )
        {
            o.planeValid = true;
            return;
        }
        const int n = (int)( o.pts.size() / 3 );
        o.planeValid = ( n >= 3 ) && KiwiCon_FitPlane( &o.pts[0], n, &o.plane );
    }

    void RefitAll()
    {
        for ( size_t i = 0; i < s_objects.size(); ++i )
            RefitPlane( s_objects[i] );
    }

    // ── KIWI-UX (ROUND R): THE SEAM RULE, ENFORCED BY THE STORE ─────────────
    // Drop consecutive coincident points and, on a CLOSED object, the repeated
    // seam vertex.  The full report and the reasoning are on KiwiCon_Add's
    // declaration in kiwi_construct.h; the tolerance is KREG_JOIN_DIST because
    // that is §8's ONE slop and a point pair the chain walker would weld is by
    // definition a point pair this store must not keep as two.
    //
    // Runs on ADD and on SIDECAR LOAD, and deliberately NOT inside
    // KiwiCon_NoteMutated: a MOVE gesture legitimately drags one point through
    // another on its way somewhere, and eating a vertex mid-drag would destroy
    // data the user is still holding.
    void NormalizePoints( kconObject_t &o )
    {
        if ( KiwiCon_IsParametric( o ) )
            return;
        const int n = (int)( o.pts.size() / 3 );
        if ( n < 2 )
            return;

        std::vector<float> out;
        out.reserve( o.pts.size() );
        for ( int i = 0; i < n; ++i )
        {
            const float *p = &o.pts[(size_t)i * 3];
            const int have = (int)( out.size() / 3 );
            if ( have > 0 )
            {
                const float dx = p[0] - out[(size_t)( have - 1 ) * 3 + 0];
                const float dy = p[1] - out[(size_t)( have - 1 ) * 3 + 1];
                const float dz = p[2] - out[(size_t)( have - 1 ) * 3 + 2];
                if ( sqrtf( dx * dx + dy * dy + dz * dz ) <= KREG_JOIN_DIST )
                    continue;
            }
            out.push_back( p[0] );  out.push_back( p[1] );  out.push_back( p[2] );
        }

        // THE SEAM.  A closed ring's wrap is implicit, so a stored last point that
        // lands on the first is one edge of zero length — the shape §19's V5 gate
        // reports as "duplicate plane" once §23 has built two side faces off it.
        // Gated on `closed` on purpose: an OPEN chain that happens to come back to
        // its own start is a legal (and drawable) shape, and silently closing it
        // would be this function deciding something the user did not.
        const int m = (int)( out.size() / 3 );
        if ( o.closed && m >= 2 )
        {
            const float dx = out[0] - out[(size_t)( m - 1 ) * 3 + 0];
            const float dy = out[1] - out[(size_t)( m - 1 ) * 3 + 1];
            const float dz = out[2] - out[(size_t)( m - 1 ) * 3 + 2];
            if ( sqrtf( dx * dx + dy * dy + dz * dz ) <= KREG_JOIN_DIST )
                out.resize( (size_t)( m - 1 ) * 3 );
        }

        if ( (int)( out.size() / 3 ) != n )
            o.pts.swap( out );
    }

    // KIWI-UX (ROUND R): §16's documented default plane — XY through the world
    // origin — written straight into the file statics rather than through
    // KiwiCon_SetActivePlane, because this runs from KiwiCon_ClearAll during a map
    // teardown and there is no repaint to schedule from there (Map_NewMap sets
    // g_nUpdateBits itself once the new map is live).  Nothing is lost if the
    // MakePlane call ever fails: s_planeSeeded is left alone and KiwiCon_ActivePlane
    // rebuilds the same default on its next read.
    void ResetActivePlaneToGround()
    {
        const float o[3] = { 0.0f, 0.0f, 0.0f };
        const float n[3] = { 0.0f, 0.0f, 1.0f };
        if ( KiwiCon_MakePlane( o, n, 0, &s_plane ) )
            s_planeSeeded = true;
    }
}

int KiwiCon_Add( const kconObject_t &o )
{
    // KIWI-UX (ROUND R): normalise BEFORE the budget checks, so a chain whose only
    // "extra" points were duplicates is measured — and accepted or refused — on the
    // points it will actually be stored with.
    kconObject_t obj = o;
    NormalizePoints( obj );
    // A tool must never be able to push a runaway object into the store — the
    // draw pass, the snap scan and the region walk are all sized off these counts.
    if ( (int)( obj.pts.size() / 3 ) > KCON_MAX_POINTS )
    {
        Sys_Printf( "Construction: object rejected (over %i points).\n", KCON_MAX_POINTS );
        return -1;
    }
    if ( KiwiCon_VertCount( obj ) < 2 )
    {
        Sys_Printf( "Construction: object rejected (fewer than 2 points).\n" );
        return -1;
    }
    s_objects.push_back( obj );
    RefitPlane( s_objects.back() );               // shakeout H: the cached fit
    Touch();
    return (int)s_objects.size() - 1;
}

bool KiwiCon_RemoveAt( int index )
{
    if ( index < 0 || index >= (int)s_objects.size() )
        return false;
    s_objects.erase( s_objects.begin() + index );
    KiwiConSel_NoteStoreReplaced();       // shakeout F — indices below this one shifted
    Touch();
    return true;
}

void KiwiCon_ClearAll()
{
    // KIWI-UX (ROUND R): the WORKING PLANE goes back to ground XY through the world
    // origin, BEFORE the early-out, because the plane is exactly the state the user
    // could not clear ("even deleting the whole scene doesn't fix it") and an empty
    // store is the commonest way to reach here.  §16's documented default is "XY at
    // the last hit height", and with the scene gone there is no last hit — so the
    // seed KiwiCon_ActivePlane itself uses is the honest answer.  The full
    // root-cause chain is on KiwiCon_SnapUV in kiwi_construct.h.
    ResetActivePlaneToGround();

    if ( s_objects.empty() )
        return;
    s_objects.clear();
    // ROUND W: the group TABLE goes with the objects.  "File -> New" means the
    // scene is gone; leaving folder names behind would make the next map's
    // outliner list groups whose members were deleted before it was loaded.
    s_groups.clear();
    s_nextGroupId = 1;
    KiwiConSel_NoteStoreReplaced();
    Touch();
}

// ── ROUND U: the HIDDEN flag (kiwi_construct.h) ─────────────────────────────
bool KiwiCon_Hidden( int index )
{
    if ( index < 0 || index >= (int)s_objects.size() )
        return true;             // out of range reads as "not visible" — see the header
    return s_objects[index].hidden;
}

void KiwiCon_SetHidden( int index, bool hidden )
{
    if ( index < 0 || index >= (int)s_objects.size() )
        return;
    if ( s_objects[index].hidden == hidden )
        return;
    s_objects[index].hidden = hidden;
    // Touch() and NOT NoteMutated(): nothing geometric moved, so the cached plane
    // fits are all still correct and a full RefitAll would be work for nothing.
    // The generation bump IS needed — the region derivation caches on it, and a
    // hidden line must drop out of the regions it was bounding.
    Touch();
    // ── KIWI-UX (ROUND X, ITEM 11): THE REPAINT ─────────────────────────────
    // USER REPORT, verbatim: "When hiding objects, curves are not hidden even
    // though they are marked as hidden. Fix this."
    //
    // The draw pass has skipped hidden objects since round U and still does; what
    // was missing is the REQUEST TO REDRAW.  The shell paints no scene unless
    // something asks (the white-flicker guard), so a store change that bumps only
    // the generation leaves the last-rendered image — with the curve still in it —
    // on screen until some unrelated act happens to invalidate.  The eye in the
    // ROUND W outliner is exactly that path: `SetBrushHidden` (kiwi_outliner.cpp:352)
    // ends in `g_nUpdateBits = -1`, so hiding a BRUSH from the panel repainted and
    // hiding a CURVE did not — which is the report, precisely, including why it
    // reads as "marked hidden but still drawn".
    //
    // It goes HERE rather than at the call sites so there is one owner: the H
    // path (kiwi_conselect.cpp KiwiConSel_HideSelected) already sets its own bit
    // and is unaffected by the duplicate.
    g_nUpdateBits = -1;
}

// ── ROUND X, ITEM 10: the per-object NAME (kiwi_construct.h) ────────────────
const char *KiwiCon_Name( int index )
{
    if ( index < 0 || index >= (int)s_objects.size() )
        return "";
    return s_objects[index].name.c_str();
}

void KiwiCon_SetName( int index, const char *name )
{
    if ( index < 0 || index >= (int)s_objects.size() )
        return;
    std::string v = name ? name : "";
    if ( (int)v.size() > KCON_NAME_MAX - 1 )
        v.resize( KCON_NAME_MAX - 1 );
    // The sidecar writes the name between quotes, so a quote inside one would make
    // the file unreadable on the next load.  Dropped rather than escaped: an escape
    // syntax is a format change and nobody needs a quote in a scaffolding label.
    for ( size_t i = 0; i < v.size(); )
        if ( v[i] == '"' ) v.erase( i, 1 ); else ++i;
    if ( s_objects[index].name == v )
        return;
    s_objects[index].name = v;
    Touch();
    g_nUpdateBits = -1;
}

bool KiwiCon_HasHidden()
{
    for ( size_t i = 0; i < s_objects.size(); ++i )
        if ( s_objects[i].hidden )
            return true;
    return false;
}

int KiwiCon_UnhideAll()
{
    int n = 0;
    for ( size_t i = 0; i < s_objects.size(); ++i )
        if ( s_objects[i].hidden )
            ++n;
    if ( !n )
        return 0;
    KiwiCon_UndoPush();                    // its own bracket — see the header
    for ( size_t i = 0; i < s_objects.size(); ++i )
        s_objects[i].hidden = false;
    Touch();
    g_nUpdateBits = -1;               // ROUND X, ITEM 11 — see KiwiCon_SetHidden
    return n;
}

// ── ROUND W: the GROUP field + the group-name table (kiwi_construct.h) ──────
// The table itself (s_groups / s_nextGroupId / GroupSlot) lives with the store at
// the head of this file, because KiwiCon_ClearAll and KiwiCon_LoadSidecar — both
// defined above this point — have to reset it.
int KiwiCon_Group( int index )
{
    if ( index < 0 || index >= (int)s_objects.size() )
        return -1;                       // out of range reads as UNGROUPED
    return s_objects[index].group;
}

void KiwiCon_SetGroup( int index, int group )
{
    if ( index < 0 || index >= (int)s_objects.size() )
        return;
    // An id nobody declared is not a group; treat it as "ungroup" rather than
    // letting a stray write mint a folder the name table knows nothing about.
    if ( group >= 0 && GroupSlot( group ) < 0 )
        group = -1;
    if ( s_objects[index].group == group )
        return;
    s_objects[index].group = group;
    // Touch() and NOT NoteMutated(), for the same reason SetHidden gives: nothing
    // geometric moved.  The generation bump is what the outliner (and anything else
    // caching a derived view) needs to notice the row moved folders.
    Touch();
}

int KiwiCon_NewGroup( const char *name )
{
    kconGroup_t g;
    memset( &g, 0, sizeof( g ) );
    g.id = s_nextGroupId++;
    if ( name && name[0] )
        _snprintf( g.name, sizeof( g.name ), "%s", name );
    else
        _snprintf( g.name, sizeof( g.name ), "Group %i", g.id );
    g.name[sizeof( g.name ) - 1] = '\0';
    s_groups.push_back( g );
    Touch();
    return g.id;
}

int KiwiCon_GroupCount()
{
    return (int)s_groups.size();
}

int KiwiCon_GroupIdAt( int i )
{
    if ( i < 0 || i >= (int)s_groups.size() )
        return -1;
    return s_groups[i].id;
}

bool KiwiCon_GroupExists( int group )
{
    return group >= 0 && GroupSlot( group ) >= 0;
}

const char *KiwiCon_GroupName( int group )
{
    const int slot = GroupSlot( group );
    return ( slot < 0 ) ? "" : s_groups[slot].name;
}

void KiwiCon_SetGroupName( int group, const char *name )
{
    const int slot = GroupSlot( group );
    if ( slot < 0 || !name || !name[0] )
        return;                          // an empty rename is a cancel, not a blank
    _snprintf( s_groups[slot].name, KCON_GROUPNAME_MAX, "%s", name );
    s_groups[slot].name[KCON_GROUPNAME_MAX - 1] = '\0';
    Touch();
}

bool KiwiCon_RemoveGroup( int group )
{
    const int slot = GroupSlot( group );
    if ( slot < 0 )
        return false;
    for ( size_t i = 0; i < s_objects.size(); ++i )
        if ( s_objects[i].group == group )
            s_objects[i].group = -1;
    s_groups.erase( s_groups.begin() + slot );
    Touch();
    return true;
}

int KiwiCon_GroupMemberCount( int group )
{
    if ( group < 0 )
        return 0;
    int n = 0;
    for ( size_t i = 0; i < s_objects.size(); ++i )
        if ( s_objects[i].group == group )
            ++n;
    return n;
}

// ── SHAKEOUT F: in-place edit access (kiwi_construct.h) ─────────────────────
kconObject_t *KiwiCon_MutableAt( int index )
{
    if ( index < 0 || index >= (int)s_objects.size() )
        return 0;
    return &s_objects[index];
}

void KiwiCon_NoteMutated()
{
    // SHAKEOUT H: a mutation may have moved points, so every cached fit is stale.
    // Refitting the WHOLE store rather than asking the caller which object it
    // touched: the caller does not always know (a Move writes several), the fit is
    // a few dozen flops per object, and a store this small is walked every frame
    // by the draw pass anyway.  One rule, no way to forget it.
    RefitAll();
    Touch();
}

unsigned KiwiCon_Generation()
{
    return s_generation;
}

bool KiwiCon_HasObjects()
{
    return !s_objects.empty();
}

// ─── the store's own undo (kiwi_construct.h ruling 2) ────────────────────────
// SHAKEOUT I: still the store's OWN snapshots and still its own depth limit —
// what changed is that the ORDER is no longer this file's business.  Every push
// mints a ticket in the unified journal (kiwi_undo.h) and every pop is driven BY
// that journal, so a construction step and a brush step share one Ctrl+Z.
void KiwiCon_UndoPush()
{
    s_undo.push_back( s_objects );
    if ( (int)s_undo.size() > KCON_UNDO_DEPTH )
        s_undo.erase( s_undo.begin() );
    // A new record destroys the redo, exactly as undo.cpp's Undo_Start does
    // (Undo_ClearRedo + Undo_GeneralStart).  Cleared HERE and not only in the
    // journal because the snapshots are this file's storage.
    s_redo.clear();
    // KIWI-UX (shakeout I): the journal ticket.  It also clears the JOURNAL's redo
    // stack, which is the same rule one level up.
    KiwiUndo_NoteConstructionRecord( "construction edit" );
}

bool KiwiCon_UndoPop()
{
    if ( s_undo.empty() )
        return false;
    // SHAKEOUT I: the CURRENT state becomes the redo snapshot before it is
    // overwritten — that, and nothing else, is what "there was no construction
    // redo" cost.  Bounded by the same KCON_UNDO_DEPTH as the undo side.
    s_redo.push_back( s_objects );
    if ( (int)s_redo.size() > KCON_UNDO_DEPTH )
        s_redo.erase( s_redo.begin() );

    s_objects = s_undo.back();
    s_undo.pop_back();
    // Shakeout F: the store is replaced WHOLESALE here, so every index the
    // construction selection holds is meaningless.  Dropping the selection is the
    // only honest answer — remapping it would need object identity, and these
    // objects have none (they are addressed by position, kiwi_conselect.h).
    KiwiConSel_NoteStoreReplaced();
    Touch();
    return true;
}

// SHAKEOUT I: the mirror image, and deliberately NOT written in terms of
// KiwiCon_UndoPush — that spelling would clear the very redo stack this is
// walking (and mint a second journal ticket for a step that already has one).
bool KiwiCon_RedoPop()
{
    if ( s_redo.empty() )
        return false;
    s_undo.push_back( s_objects );
    if ( (int)s_undo.size() > KCON_UNDO_DEPTH )
        s_undo.erase( s_undo.begin() );

    s_objects = s_redo.back();
    s_redo.pop_back();
    KiwiConSel_NoteStoreReplaced();       // same wholesale-replacement rule as UndoPop
    Touch();
    return true;
}

void KiwiCon_ClearRedo()
{
    s_redo.clear();
}

int KiwiCon_UndoDepth()
{
    return (int)s_undo.size();
}

int KiwiCon_RedoDepth()
{
    return (int)s_redo.size();
}

// ─── the active construction plane (§16) ─────────────────────────────────────
const kconPlane_t &KiwiCon_ActivePlane()
{
    if ( !s_planeSeeded )
    {
        // The documented default: XY through the world origin (§16 "XY at last
        // hit height", and the last hit height is 0 until something is placed).
        const float o[3] = { 0.0f, 0.0f, 0.0f };
        const float n[3] = { 0.0f, 0.0f, 1.0f };
        KiwiCon_MakePlane( o, n, 0, &s_plane );
        s_planeSeeded = true;
    }
    return s_plane;
}

void KiwiCon_SetActivePlane( const kconPlane_t &p )
{
    s_plane       = p;
    s_planeSeeded = true;
    g_nUpdateBits |= 1;
}

// ═════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND AT, ITEM 2b) — A MAJOR PLANE'S OFFSET IS A *WORKING HEIGHT*
// ═════════════════════════════════════════════════════════════════════════════
// USER REPORT, verbatim: "when I lock the camera to TOP, it doesn't draw them on
// the major plane either.  IT draws them slightly above it??"
//
// THE THREE RUNGS THAT SYNTHESISE AN AXIS PLANE — the Z cycle (this function),
// rung 4 (view-dominant) and rung 0 (axis view) — all built it as
//     origin = the PREVIOUS plane's origin, verbatim
// and that is where "slightly above" comes from.  The previous origin is not a
// height the user chose; it is whatever the last plane-setter happened to leave:
//   * rung 3's `pick.point` — the RAW ray hit on a face, an arbitrary point under
//     the cursor, off-grid by construction (kiwi_construct.h's round-R note says
//     so in those words);
//   * rung 2's winding CENTROID;
//   * rung 1's construction-object origin;
//   * `PushPoint`'s placed point, which may have come off a geometry snap.
// None of those is quantised, either: `KiwiCon_SnapUV` snaps the two IN-PLANE
// coordinates and deliberately never touches the normal component (the header
// states that), so an off-grid offset survives every snap forever.  Round R fixed
// the same poisoning for the LATTICE by anchoring it at the world origin; the
// plane's own OFFSET was left alone and is the other half of that bug.
//
// TWO RULES, AND THEY ARE THE ONLY TWO THIS NEEDS:
//
//   1. AN OFFSET IS ONLY INHERITED FROM A PARALLEL PLANE.  If the plane being
//      replaced is parallel to the one being built, its offset along that axis IS
//      the height being worked at and keeping it is round T/Y's documented intent
//      ("XY after a placement at z = 128 means the plane THROUGH that placement").
//      If it is NOT parallel — a tilted face plane, or an XZ plane when XY is
//      being built — then `origin[axis]` is not a working height at all, it is the
//      arbitrary coordinate of a point that happened to be on some other plane.
//      There is no height to inherit, so the MAJOR plane is the world's own: 0.
//   2. THE OFFSET IS GRID-QUANTISED.  A working height is a number the user could
//      type; 64.3271 is not.  Snapping it here means a major plane can never sit a
//      fraction of a unit above the ground again, whatever seeded it.  With the
//      round-AJ grid-snap master switch OFF, `KiwiGrid_Snap` copies through — the
//      same "leave the value alone" answer every other caller already handles.
//
// The two off-axis components are carried through untouched: they do not affect
// the plane at all (a plane is normal + offset), and they keep the working-plane
// OVERLAY square drawn where the work is instead of teleporting it to the world
// origin.
static float KiwiCon_MajorPlaneOffset( int axis )
{
    if ( axis < 0 || axis > 2 )
        return 0.0f;
    const kconPlane_t &cur = KiwiCon_ActivePlane();
    float h = 0.0f;
    if ( fabsf( cur.normal[axis] ) > KCON_PLANE_PARALLEL )      // rule 1
        h = cur.origin[axis];
    float in[3]  = { 0.0f, 0.0f, 0.0f };
    float out[3] = { 0.0f, 0.0f, 0.0f };
    in[axis] = h;
    KiwiGrid_Snap( in, out );                                    // rule 2
    return out[axis];
}

// The same rule, applied to a whole origin: keep the off-axis components, replace
// the axis one.  Every synthesised major plane goes through this.
static void KiwiCon_MajorPlaneOrigin( int axis, float out[3] )
{
    const kconPlane_t &cur = KiwiCon_ActivePlane();
    Copy3( cur.origin, out );
    if ( axis >= 0 && axis <= 2 )
        out[axis] = KiwiCon_MajorPlaneOffset( axis );
}

void KiwiCon_SetPlaneAxis( int axis )
{
    if ( axis < 0 || axis > 2 )
        return;
    // Keep the plane where it already is along its new normal, so "XY" after a
    // placement at z = 128 means the plane THROUGH that placement, not z = 0 —
    // but only when the plane being replaced is PARALLEL, which is exactly when
    // that sentence is true (ROUND AT, ITEM 2b, above).
    float o[3];
    KiwiCon_MajorPlaneOrigin( axis, o );
    float n[3] = { 0.0f, 0.0f, 0.0f };
    n[axis] = 1.0f;

    kconPlane_t p;
    if ( !KiwiCon_MakePlane( o, n, 0, &p ) )
        return;
    KiwiCon_SetActivePlane( p );
    Sys_Printf( "Construction plane: %s at %g\n",
                ( axis == 2 ) ? "XY" : ( axis == 1 ) ? "XZ" : "YZ", (double)o[axis] );
}

void KiwiCon_SetPlaneFromView()
{
    CamWnd_BuildMatrix();
    camera_s *c = Ed_Camera();

    // A view plane needs a DEPTH: put it through the current plane's origin so
    // "from view" re-aims the plane without teleporting it to the eye.
    const kconPlane_t &cur = KiwiCon_ActivePlane();
    float n[3];
    Copy3( c->vpn, n );
    n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2];     // face the camera

    kconPlane_t p;
    if ( !KiwiCon_MakePlane( cur.origin, n, c->vright, &p ) )
        return;
    KiwiCon_SetActivePlane( p );
    Sys_Printf( "Construction plane: from view.\n" );
}

namespace
{
    // ═════════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND AT, ITEM 2a) — "IS THIS FACE SOMETHING I CAN DRAW ON FROM
    //  HERE?", THE QUESTION RUNGS 2 AND 3 NEVER ASKED
    // ═════════════════════════════════════════════════════════════════════════
    // USER REPORT: lines drawn in open space, below and away from the geometry,
    // at an oblique camera, land on a TILTED plane.  "The construction plane here
    // should be flat."
    //
    // Both face rungs answered from a fact that is not about where the user is
    // pointing:
    //   * rung 2 (round U) fires on SELECTION STATE ALONE.  A face selected
    //     earlier — and a face selection is sticky — hands back its plane no
    //     matter where the cursor is, including thousands of units away in open
    //     sky.  The sketch then lands on that face's plane extended out to
    //     nowhere, which is exactly a tilted plane under a click that touched
    //     nothing.
    //   * rung 3 (§16) fires on any face the ray reaches, with no distance bound
    //     and no angle test.  From an oblique camera a ray aimed at empty space
    //     below the geometry still crosses a distant wall or roof at a grazing
    //     angle, and that face wins.
    // Rung 4 — the view-dominant MAJOR plane, always axis-aligned, which is what
    // the user is asking for — is below both of them and is therefore never
    // reached in either case.
    //
    // TWO TESTS, and a face has to pass both:
    //   FACING.  |n . raydir| must clear KCON_PLANE_FACING_MIN.  A plane you are
    //   looking along the edge of is not a plane you can place points on: the
    //   pixels-to-world scale goes to infinity as it turns edge-on.  This is the
    //   whole of rung 3's defect and half of rung 2's.
    //   REACH (rung 2 only).  The cursor ray must meet the face's plane inside the
    //   face's own bounds grown by KCON_FACE_PLANE_SLACK of its largest extent.
    //   Round U's flow — select a face, draw a window on it — puts the cursor ON
    //   the face, and its documented tolerance ("the cursor drifted onto the floor
    //   behind it") is a drift of a face-width, not of a map.  Rung 3 needs no
    //   reach test: its face is under the cursor by construction.
    //
    // A refusal is a FALL-THROUGH, not an error: the ladder simply carries on to
    // the next rung, and with both face rungs declining it reaches rung 4 and the
    // user gets the flat major plane they asked for.
    bool FaceFacesTheRay( const face_t *f, const ray_t &ray )
    {
        const float den = Dot3( ray.dir, f->plane.normal );
        return ( fabsf( den ) >= KCON_PLANE_FACING_MIN );
    }

    bool RayReachesFace( const face_t *f, const winding_t *w, const ray_t &ray )
    {
        if ( !w || w->numpoints < 3 || w->numpoints > MAX_POINTS_ON_WINDING )
            return false;
        const float den = Dot3( ray.dir, f->plane.normal );
        if ( fabsf( den ) < KCON_PLANE_FACING_MIN )
            return false;

        float rel[3];
        Sub3( w->p[0], ray.origin, rel );
        const float t = Dot3( rel, f->plane.normal ) / den;
        if ( !( t > 0.0f ) || t > KCON_PLANE_REACH )
            return false;
        float hit[3];
        Mad3( ray.origin, ray.dir, t, hit );

        float mins[3], maxs[3];
        Copy3( w->p[0], mins );
        Copy3( w->p[0], maxs );
        for ( int i = 1; i < w->numpoints; ++i )
            for ( int k = 0; k < 3; ++k )
            {
                if ( w->p[i][k] < mins[k] ) mins[k] = w->p[i][k];
                if ( w->p[i][k] > maxs[k] ) maxs[k] = w->p[i][k];
            }
        float extent = 0.0f;
        for ( int k = 0; k < 3; ++k )
            if ( maxs[k] - mins[k] > extent )
                extent = maxs[k] - mins[k];
        float grow = extent * KCON_FACE_PLANE_SLACK;
        if ( grow < KCON_FACE_PLANE_MINGROW )
            grow = KCON_FACE_PLANE_MINGROW;

        for ( int k = 0; k < 3; ++k )
            if ( hit[k] < mins[k] - grow || hit[k] > maxs[k] + grow )
                return false;
        return true;
    }

    // §16: origin = the hit point, normal = the face normal, u = the winding's
    // LONGEST edge.  The longest edge is the numerically safest in-plane
    // direction available, and it makes the plane's grid line up with the face
    // the user is drawing on instead of with the world.
    bool PlaneFromFacePick( const pick_result_t &pick, const ray_t &ray, kconPlane_t *out )
    {
        if ( !pick.valid || pick.item.kind != SEL_FACE )
            return false;
        selbrush_t *node = pick.item.brush;
        if ( !node || !node->def || !node->def->faces )
            return false;
        const int fi = pick.item.faceIndex;
        if ( fi < 0 || fi >= node->def->faceCount )
            return false;
        face_t *f = &node->def->faces[fi];

        // KIWI-UX (ROUND AT, ITEM 2a): a face turned edge-on to the view is not a
        // surface this rung may hand back — see the block above.
        if ( !FaceFacesTheRay( f, ray ) )
            return false;

        const float *hint = 0;
        float longest[3] = { 0.0f, 0.0f, 0.0f };
        winding_t *w = f->w;
        if ( w && w->numpoints >= 2 && w->numpoints <= MAX_POINTS_ON_WINDING )
        {
            float bestLen = 0.0f;
            for ( int i = 0; i < w->numpoints; ++i )
            {
                float e[3];
                Sub3( w->p[( i + 1 ) % w->numpoints], w->p[i], e );
                const float l = Len3( e );
                if ( l > bestLen )
                {
                    bestLen = l;
                    Copy3( e, longest );
                }
            }
            if ( bestLen > 1.0e-3f )
                hint = longest;
        }
        return KiwiCon_MakePlane( pick.point, f->plane.normal, hint, out );
    }
}

bool KiwiCon_SetPlaneFromCursorFace()
{
    ray_t ray;
    if ( !Pick_RayFromCursor( &ray ) )
        return false;
    const pick_result_t pick = Pick( ray, SEL_MASK_FACE );
    kconPlane_t p;
    if ( !PlaneFromFacePick( pick, ray, &p ) )
        return false;
    KiwiCon_SetActivePlane( p );
    return true;
}

// ── KIWI-UX (ROUND U): THE **SELECTED** FACE OWNS THE WORKING PLANE ──────────
// USER DIRECTIVE, verbatim: "I should be able to press Shift-A (or similar) while
// a face is selected and in the extrusion mode.  The idea is to select a face,
// shift-A, and then start drawing lines on the face so that I can do an
// extrusion+boolean on it for something like a window."
//
// The second half of that flow (the preempt) is kiwi_command.cpp's PreemptVerb;
// this is the first half.  The face SURVIVES the record-free cancel, so at the
// moment the line tool's Begin() runs, the selection still holds exactly the face
// the user picked — and that is a stronger statement about where to draw than
// anything derived from a ray.
//
// EXACTLY ONE FACE, and no more: with two selected there is no single answer, and
// silently picking one of them is how a sketch lands on the wrong wall.  The
// origin is the winding CENTRE (not a ray hit — there is no ray here) and u is the
// winding's longest edge, the same two choices PlaneFromFacePick makes, so a
// sketch started this way and one started by pointing at the same face get the
// same lattice.
bool KiwiCon_SetPlaneFromSelectedFace()
{
    const selection_t &sel = KiwiSel();
    const sel_item_t  *face = 0;
    for ( size_t i = 0; i < sel.items.size(); ++i )
    {
        if ( sel.items[i].kind != SEL_FACE )
            continue;
        if ( face )
            return false;                     // more than one: no single answer
        face = &sel.items[i];
    }
    if ( !face || !Sel_BrushLive( face->brush ) )
        return false;

    selbrush_t *node = face->brush;
    if ( !node->def || !node->def->faces )
        return false;
    const int fi = face->faceIndex;
    if ( fi < 0 || fi >= node->def->faceCount )
        return false;
    face_t    *f = &node->def->faces[fi];
    winding_t *w = f->w;
    if ( !w || w->numpoints < 3 || w->numpoints > MAX_POINTS_ON_WINDING )
        return false;

    // ── KIWI-UX (ROUND AT, ITEM 2a): THE SELECTION IS NOT ENOUGH ─────────────
    // A face selection is sticky and this rung used to fire on it alone, so a face
    // picked minutes ago captured a sketch drawn anywhere at all — including in
    // open space at an oblique camera, which is the tilted plane in the report.
    // The face now has to be somewhere the user could actually be drawing on it:
    // facing the camera at all, and under (or within a face-width of) the cursor.
    // See the FaceFacesTheRay / RayReachesFace block above for both tests and for
    // why the tolerance is a face-width rather than a pixel radius.
    //
    // WITH NO CURSOR RAY AT ALL — the command was started from a menu, say — the
    // question cannot be asked and the round-U answer stands unchanged.  Refusing
    // there would break the flow this rung exists for.
    ray_t ray;
    if ( Pick_RayFromCursor( &ray ) && !RayReachesFace( f, w, ray ) )
        return false;

    float centre[3] = { 0.0f, 0.0f, 0.0f };
    for ( int i = 0; i < w->numpoints; ++i )
        for ( int k = 0; k < 3; ++k )
            centre[k] += w->p[i][k];
    for ( int k = 0; k < 3; ++k )
        centre[k] /= (float)w->numpoints;

    const float *hint = 0;
    float longest[3] = { 0.0f, 0.0f, 0.0f };
    float bestLen    = 0.0f;
    for ( int i = 0; i < w->numpoints; ++i )
    {
        float e[3];
        Sub3( w->p[( i + 1 ) % w->numpoints], w->p[i], e );
        const float l = Len3( e );
        if ( l > bestLen ) { bestLen = l; Copy3( e, longest ); }
    }
    if ( bestLen > 1.0e-3f )
        hint = longest;

    kconPlane_t p;
    if ( !KiwiCon_MakePlane( centre, f->plane.normal, hint, &p ) )
        return false;
    KiwiCon_SetActivePlane( p );
    return true;
}

// ── SHAKEOUT F: "continue what I was drawing" beats "the face I am pointing at" ─
// USER REPORT this round: a loop drawn as separate lines on a NON-axis-aligned
// face does not become a region.  The chain/plane machinery in kiwi_region turned
// out to be sound (see the SamePlane note there — the plane constant is derived
// from the origin every time, so two objects derived from the same face at
// different cursor points compare EQUAL).  THIS is where it actually broke.
//
// Every drawing tool re-derives its plane from the face under the cursor at
// Begin().  Draw line 1 on a slanted face, then start line 2 from line 1's
// endpoint: if that endpoint sits over a DIFFERENT face — an adjacent wall, the
// floor behind the brush, anything the ray reaches first once the cursor has
// moved to the corner — the new tool silently adopts THAT face's plane.  The two
// lines are then not coplanar, SamePlane rejects them, no chain, no region, and
// nothing on screen says why.
//
// The fix is the rule a user would state themselves: if the cursor is ON an
// existing construction object (within the POINT radius of one of its anchors, or
// the EDGE radius of one of its segments), the new tool inherits THAT OBJECT'S
// plane.  Pointing somewhere else still derives from the face, so deliberately
// changing planes is unaffected — you change plane by starting somewhere else,
// which is exactly what starting somewhere else means.
static bool KiwiCon_PlaneFromCursorConstruction()
{
    int   curX = 0, curY = 0, w = 0, h = 0;
    if ( !ImGuiShell_CameraPaintCursor( &curX, &curY, &w, &h ) )
        return false;

    const int count = KiwiCon_Count();
    int   best     = -1;
    float bestDist = 0.0f;

    for ( int i = 0; i < count; ++i )
    {
        const kconObject_t *o = KiwiCon_At( i );
        if ( !o )
            continue;

        // Anchors first, at the point radius — the endpoint a continuing chain is
        // actually aimed at.
        const int anchors = KiwiCon_AnchorCount( *o );
        for ( int a = 0; a < anchors; ++a )
        {
            float p[3], px, py;
            if ( !KiwiCon_AnchorWorld( *o, a, p ) )
                break;
            if ( !Pick_WorldToImage( p, &px, &py ) )
                continue;
            const float dx = px - (float)curX, dy = py - (float)curY;
            const float d  = sqrtf( dx * dx + dy * dy );
            if ( d > PICK_VERT_PIXELS )
                continue;
            if ( best < 0 || d < bestDist ) { best = i; bestDist = d; }
        }

        // …then the segments, at the edge radius.
        const int segs = KiwiCon_SegmentCount( *o );
        for ( int s = 0; s < segs; ++s )
        {
            float wa[3], wb[3], ax, ay, bx, by;
            if ( !KiwiCon_SegmentWorld( *o, s, wa, wb ) )
                break;
            if ( !Pick_WorldToImage( wa, &ax, &ay ) || !Pick_WorldToImage( wb, &bx, &by ) )
                continue;
            const float ex = bx - ax, ey = by - ay;
            const float len2 = ex * ex + ey * ey;
            float t = 0.0f;
            if ( len2 > 1.0e-6f )
            {
                t = ( ( (float)curX - ax ) * ex + ( (float)curY - ay ) * ey ) / len2;
                if ( t < 0.0f ) t = 0.0f;
                if ( t > 1.0f ) t = 1.0f;
            }
            const float dx = ax + ex * t - (float)curX;
            const float dy = ay + ey * t - (float)curY;
            const float d  = sqrtf( dx * dx + dy * dy );
            // KIWI-UX (ROUND K): KCON_LINE_PIXELS, so "continue the object under the
            // cursor" reaches as far as clicking it does.
            if ( d > KCON_LINE_PIXELS )
                continue;
            if ( best < 0 || d < bestDist ) { best = i; bestDist = d; }
        }
    }

    if ( best < 0 )
        return false;
    // SHAKEOUT H: an object's plane is DERIVED now and may not exist at all (a
    // non-planar polyline has none).  Inheriting nothing is the honest answer —
    // the active plane simply stands, which is what pointing at empty space does.
    kconPlane_t p;
    if ( !KiwiCon_ObjectPlane( *KiwiCon_At( best ), &p ) )
        return false;
    KiwiCon_SetActivePlane( p );
    return true;
}

// ── KIWI-UX (ROUND T): THE VIEW-DOMINANT WORLD AXIS ─────────────────────────
// USER DIRECTIVE, verbatim: "I can create a vertical cylinder, but now I can't
// do a horizonal one!  The starting circle needs to orient itself based on the
// camera, and many other tools should operate with this orientation in mind.
// See how plasticity works."
//
// The working plane's normal becomes THE WORLD AXIS MOST ALIGNED WITH THE VIEW
// DIRECTION: look down and you draw on XY, face a wall along X and you draw on
// YZ, face one along Y and you draw on XZ.  So "face the wall, draw a circle,
// pull it toward you" just works, and the cylinder primitive — which refuses a
// non-axis-aligned plane outright because the ported Brush_MakeSided takes a
// WORLD axis (kiwi_primitive.cpp) — gets a plane it can always use.
//
// WHY THE DOMINANT AXIS AND NOT THE VIEW PLANE ITSELF.  KiwiCon_SetPlaneFromView
// (above) already offers the exact view plane, and it is deliberately NOT what
// this rung does: an arbitrarily-angled plane is refused by the cylinder and the
// cone, makes every drawn shape off-axis, and is almost never what a mapper
// means by "draw on that wall".  Quantising to the nearest world axis is what
// Plasticity's construction-plane snapping does and what a brush editor needs.
//
// RECOMPUTED AT TOOL START ONLY — it is called from KiwiCon_AutoPlaneForTool,
// which runs in Begin().  Orbiting mid-gesture must not re-seat a plane the user
// has already placed points on, and the Z cycle stays the explicit override.
//
// The plane keeps its CURRENT position along the new normal, exactly as the Z
// cycle's KiwiCon_SetPlaneAxis does, so "YZ" means the YZ plane through where
// you were working rather than one teleported to x = 0.
bool KiwiCon_SetPlaneFromViewDominantAxis()
{
    CamWnd_BuildMatrix();
    const camera_s *c = Ed_Camera();
    if ( !c )
        return false;

    int   axis = 2;
    float best = fabsf( c->vpn[2] );
    for ( int k = 0; k < 2; ++k )
        if ( fabsf( c->vpn[k] ) > best ) { best = fabsf( c->vpn[k] ); axis = k; }
    if ( !( best > 1.0e-4f ) )
        return false;                       // a degenerate view matrix; leave it alone

    // KIWI-UX (ROUND AT, ITEM 2b): the offset is a working height, not the last
    // origin — inherited only from a PARALLEL plane, and grid-quantised.
    float o[3];
    KiwiCon_MajorPlaneOrigin( axis, o );
    float n[3] = { 0.0f, 0.0f, 0.0f };
    n[axis] = 1.0f;

    kconPlane_t p;
    if ( !KiwiCon_MakePlane( o, n, 0, &p ) )
        return false;
    KiwiCon_SetActivePlane( p );
    Sys_Printf( "Construction plane: %s (from the view) at %g\n",
                ( axis == 2 ) ? "XY" : ( axis == 1 ) ? "XZ" : "YZ", (double)o[axis] );
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
//  ROUND Y, ITEM 4 — AN AXIS VIEW OWNS THE PLANE
// ═════════════════════════════════════════════════════════════════════════════
// USER DIRECTIVE, verbatim: "the line tool is still way out of whack.  IT doesn't
// respect the camera angle.  When snapping to top or bottom it is IMPOSSIBLE for
// me to represent a Z direction.  Take that into account and apply it on the
// other views too (see how plasticity does it)."
//
// ROUND T ALREADY DERIVED A PLANE FROM THE CAMERA — and it was rung FOUR, below
// three rungs that answer from geometry.  So the reported failure is not "the
// camera is ignored", it is "the camera loses".  Snap to FRONT, put the cursor
// anywhere over the floor or select a floor face, start the line tool, and rung 2
// or rung 3 hands back XY — the one plane in which Z cannot be drawn at all,
// from the one camera angle where the user is unambiguously asking for XZ.
// The same trap on TOP/BOTTOM is what the directive names.
//
// PLASTICITY DOES NOT ARBITRATE THIS, IT DECIDES IT.  Navigating to an axis view
// enters ortho mode, and in ortho mode the construction plane becomes a hard
// restriction whether or not anything else asked for one —
// `else if (this._restriction === undefined && isOrtho) return
// baseConstructionPlane;` (plasticity/src/command/point-picker/
// PointPickerModel.ts:61-62) — while FACE snaps are dropped from the candidate
// set outright: `if (isOrthoMode) results = results.filter(r => !(r.snap
// instanceof FaceSnap))` in spirit at
// plasticity/src/editor/snaps/SnapPickerStrategy.ts:95-109 (`:98`), and every
// surviving snap is re-oriented into the plane's own basis,
// `const effectiveOrientation = isOrthoMode ? constructionPlaneOrientation :
// orientation;` (SnapPickerStrategy.ts:82).  In other words: on an axis view, the
// face under the cursor stops being an answer to "which plane".
//
// SO THE RUNG IS PROMOTED, not re-weighted.  When the camera is on an axis view
// (KiwiViewCube_ViewAxis — the live cos(3 deg) test, no latch), the view plane is
// taken FIRST and the two face rungs are skipped entirely.  Off an axis view
// nothing changes at all: the ladder is round T's, in round T's order.
//
// RUNG 1 SURVIVES, CONDITIONALLY.  Continuing the chain under the cursor is
// Plasticity's `restrictionPlane`, which outranks even ortho mode
// (PointPickerModel.ts:68).  But a chain drawn in some OTHER plane cannot be
// continued in this view either — that is the same "you cannot draw Z from the
// top" the directive is about — so it is honoured only when its plane is PARALLEL
// to the view plane, which is precisely when continuing is possible.  Its offset
// along the axis is then kept, so a sketch stays coplanar with itself.
//
// The plane's POSITION along the axis is otherwise the active plane's, exactly as
// KiwiCon_SetPlaneAxis and rung 4 already do it — which is Plasticity's ortho
// rule as well (`if (isOrtho && pickedPointSnaps.length > 0) constructionPlane =
// constructionPlane.move(last.point)`, PointPickerModel.ts:73-78: the plane
// follows the work rather than snapping back to the world origin).
static bool KiwiCon_SetPlaneFromAxisView()
{
    int   axis = 2;
    float sign = 1.0f;
    if ( !KiwiViewCube_ViewAxis( &axis, &sign ) )
        return false;

    // Rung 1, conditionally: adopt the construction object under the cursor only
    // when its plane is PARALLEL to the view plane (|n . axis| ~ 1).  That keeps a
    // multi-segment sketch coplanar with itself — including its OFFSET along the
    // axis, which is the whole reason to take it rather than to re-derive.
    //
    // KIWI-UX (ROUND AT, ITEM 2b): THE PROBE PUTS THE PLANE BACK WHEN IT LOSES.
    // KiwiCon_PlaneFromCursorConstruction WRITES the active plane before this test
    // can reject it, so a non-parallel object used to leave its own plane installed
    // and the fall-through below then read ITS origin as "where the work is".  That
    // is a height the user never chose, arriving by a path that had already been
    // ruled irrelevant.  A probe that loses must leave no trace.
    const kconPlane_t before = KiwiCon_ActivePlane();
    if ( KiwiCon_PlaneFromCursorConstruction() )
    {
        const kconPlane_t &p = KiwiCon_ActivePlane();
        if ( fabsf( p.normal[axis] ) > KCON_PLANE_PARALLEL )
        {
            Sys_Printf( "Construction plane: continuing the object under the cursor "
                        "(%s view).\n", KiwiViewCube_ViewAxisName() );
            return true;
        }
        KiwiCon_SetActivePlane( before );
    }

    // KIWI-UX (ROUND AT, ITEM 2b): a working height, inherited only from a
    // PARALLEL plane and grid-quantised — so TOP over empty space is z = 0 exactly
    // unless the user was already working at a height on this same plane.
    float o[3];
    KiwiCon_MajorPlaneOrigin( axis, o );
    float n[3] = { 0.0f, 0.0f, 0.0f };
    n[axis] = 1.0f;

    kconPlane_t p;
    if ( !KiwiCon_MakePlane( o, n, 0, &p ) )
        return false;
    KiwiCon_SetActivePlane( p );
    Sys_Printf( "Construction plane: %s, from the %s view, at %g\n",
                ( axis == 2 ) ? "XY" : ( axis == 1 ) ? "XZ" : "YZ",
                KiwiViewCube_ViewAxisName() ? KiwiViewCube_ViewAxisName() : "axis",
                (double)o[axis] );
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND AA, ITEM 6) — WHEN THIS RUNS, AND WHY THAT IS THE HYSTERESIS
// ═════════════════════════════════════════════════════════════════════════════
// USER REPORT, verbatim: "line plane recognition could use some more work.  Seems
// iffy."
//
// The obvious reading of "iffy" is that the rungs below are re-evaluated as the
// cursor moves and the plane flips at every face boundary.  THEY ARE NOT: this
// function is called from exactly two places (KiwiDrawTool::Begin and
// kiwi_primitive.cpp's Begin), i.e. ONCE, at the moment the tool starts, and the
// tool then works off its own m_plane copy for the whole gesture.  So the plane
// cannot flip mid-hover, and a per-frame hysteresis band would be damping
// something that does not oscillate.  Checked rather than assumed — this note
// exists so the next round does not add one.
//
// What WAS unstable was one rung further down: the tool took `snap.position`
// verbatim for the free-3D tools, so successive POINTS came off different
// surfaces even though the plane never moved.  That is fixed at the point of use
// (see the chain latch in KiwiDrawTool::MouseMove), which is the honest place for
// it, and it is Plasticity's own shape: every fixed-shape command there calls
// `pointPicker.restrictToPlaneThroughPoint( p1, snap )` the instant the first
// point lands — RectangleCommand.ts:65, BoxCommand.ts:82, CircleCommand.ts:32,
// PolygonCommand.ts:35, CylinderCommand.ts:30, SphereCommand.ts:18,
// EllipseCommand.ts:17, SpiralCommand.ts:29 — and `restrictionFor` on a face
// hands back a hard PlaneSnap on that face's normal (Snaps.ts:347-351).
//
// PLASTICITY HAS NO HYSTERESIS ANYWHERE, and it does not need one for a reason
// worth writing down: its construction plane NEVER auto-derives from the surface
// under the cursor.  The plane changes only through an explicit act — a keybinding
// or a navigation (Viewport.tsx:449-460, :553-566, ConstructionPlaneGenerator's
// callers) — so there is nothing to damp.  Their nearest thing to an auto-derive
// is the point picker's FACE PREFERENCE, latched to the FIRST picked point
// (PointPickerModel.ts:89-101) and RELEASED the instant the cursor leaves that
// face (SnapPickerStrategy.ts:50-56, :78-79) — the opposite of hysteresis.
//
// KIWI keeps the auto-derive, because §16's "kills most explicit management" is
// the whole value of it, and confines it to the one instant the tool starts.
// That is the same stability Plasticity gets, arrived at from the other side: not
// "never derive", but "derive once, then never again for this gesture".
void KiwiCon_AutoPlaneForTool()
{
    // ── ROUND Y, ITEM 4: RUNG 0 — an AXIS VIEW owns the plane ───────────────
    // See the block above.  On an axis-snapped camera this takes the whole
    // decision and the face rungs never run; off one, nothing below changes.
    if ( KiwiCon_SetPlaneFromAxisView() )
        return;

    // §16's "kills most explicit management", as it now stands — FIVE rungs, most
    // specific first, each one a strictly better answer than the one below it:
    //
    //   1. a construction object under the cursor  -> its plane      (shakeout F)
    //   2. exactly ONE brush face SELECTED         -> its plane      (ROUND U)
    //   3. a brush FACE under the cursor           -> its plane      (§16)
    //   4. the VIEW-DOMINANT world axis            -> XY / XZ / YZ   (ROUND T)
    //   5. nothing                                 -> the active plane stands,
    //                                                 which is XY at the last
    //                                                 placement height
    //
    // Rung 2 is ROUND U's and sits where it does deliberately.  ABOVE rung 3
    // because SELECTING a face is a deliberate act and a face merely under the
    // cursor is incidental — with a face selected and the cursor drifted onto the
    // floor behind it, "draw on the face I selected" is the only reading.  BELOW
    // rung 1 because continuing an existing chain is more specific still, and
    // because rung 1 is what keeps a multi-segment sketch coplanar (see the
    // shakeout-F note on KiwiCon_PlaneFromCursorConstruction).
    //
    // Rung 4 slots BELOW all three "you are pointing at / holding something"
    // answers on purpose: those are explicit statements about where to draw and
    // must keep outranking a guess made from the camera.  Rung 5 is reached only
    // when the view matrix itself is degenerate.
    if ( KiwiCon_PlaneFromCursorConstruction() )
    {
        Sys_Printf( "Construction plane: continuing the object under the cursor.\n" );
        return;
    }
    if ( KiwiCon_SetPlaneFromSelectedFace() )
    {
        Sys_Printf( "Construction plane: from the SELECTED face.\n" );
        return;
    }
    if ( KiwiCon_SetPlaneFromCursorFace() )
    {
        Sys_Printf( "Construction plane: from face under cursor.\n" );
        return;
    }
    KiwiCon_SetPlaneFromViewDominantAxis();
}

// ═════════════════════════════════════════════════════════════════════════════
//  The drawing tools (§7 item 22).  One base class holds everything the five
//  share: the active plane, the point chain, the snap→plane projection, the
//  numeric field, the Esc ladder and the store's own Ctrl+Z.
// ═════════════════════════════════════════════════════════════════════════════
namespace
{
    class KiwiDrawTool;
    KiwiDrawTool *s_activeTool = 0;

    // ── shakeout E: the numeric FIELD tables (kiwi_command.h NumericFields) ──
    // STATIC storage — the numeric layer copies the structs but never the labels.
    // Every drawing tool gets "length"; the BEARING tools (line / polyline) get a
    // second "angle" field, because they are the two whose current segment has an
    // unambiguous in-plane bearing.  The area tools (rect / circle / polygon /
    // arc / spline) do not: their m_cur is a CORNER, a RADIUS endpoint or one
    // control point of a curve, and "the angle of that" would mean a different
    // thing in each — the exact ambiguity that makes a grammar untrustworthy.
    const kiwiNumField_t KCON_FIELDS_LEN[1] =
    { { "length", KNUM_LENGTH, false } };
    const kiwiNumField_t KCON_FIELDS_LEN_ANG[2] =
    { { "length", KNUM_LENGTH, false },
      { "angle",  KNUM_ANGLE,  false } };
    // ── KIWI-UX (ROUND AF, ITEM 7) ──────────────────────────────────────────
    // USER DIRECTIVE, verbatim: "I want another option for side-density on the
    // round shapes.  Sometimes you want more sides on the circles/cylinders.  Add
    // this to the [tab] typing menu for the tools that use segments like that."
    //
    // The ROUND tools (circle, 2-point circle, arc, n-gon) get a second field that
    // is a COUNT, not an angle — they are exactly the tools the note above says do
    // not have an unambiguous bearing, so the slot was free.  The n-gon already had
    // `[` / `]`; those keep working and now drive the same number the field shows,
    // which is the point of putting it in the Tab menu rather than beside it.
    const kiwiNumField_t KCON_FIELDS_LEN_SIDES[2] =
    { { "length", KNUM_LENGTH, false },
      { "sides",  KNUM_COUNT,  false } };

    class KiwiDrawTool : public KiwiEditorCommand
    {
    public:
        // A drawing tool must NEVER auto-commit on a click (kiwi_command.h's
        // WantsClicks contract) — placing a point is what a click means here.
        bool WantsClicks() const override { return true; }

        const char *HudStatus() const override { return m_hud[0] ? m_hud : 0; }

        // ── SHAKEOUT I: the tool's OWN keys, for the bottom-left prompt strip ──
        // Neither of these was advertised anywhere before this round, and the Z
        // vertical constraint in particular is the whole answer to shakeout H's
        // "the lines are only in 2D" — a feature nobody can find is a feature
        // nobody has.  The framework's own keys (confirm / cancel / Tab / digits)
        // are derived by kiwi_hints.cpp and deliberately NOT repeated here.
        //
        // STATIC storage, per kiwi_command.h's HudPrompts contract: the strip
        // copies the structs but not the strings.  The Z chip is offered only on
        // the tools that honour it (PlanarOnly() refuses it on rect/circle/arc/
        // n-gon — a corner 128 units off the plane is not a thing those shapes can
        // mean), so the strip never advertises a key that prints a refusal.
        int HudPrompts( const kiwiPrompt_t **out ) const override
        {
            static const kiwiPrompt_t s_free[] = {
                { "Z",   "Vertical" },
                { "Esc", "Clear chain" },
            };
            static const kiwiPrompt_t s_planar[] = {
                { "Esc", "Clear chain" },
            };
            if ( PlanarOnly() )
            {
                *out = s_planar;
                return (int)( sizeof( s_planar ) / sizeof( s_planar[0] ) );
            }
            *out = s_free;
            return (int)( sizeof( s_free ) / sizeof( s_free[0] ) );
        }

        // ── shakeout E: named fields, the live readouts and the bubble anchor ──
        // A tool that wants the editable BEARING says so here; see the field
        // tables above for why only the two line tools do.
        virtual bool WantsAngleField() const { return false; }

        // ROUND AF, ITEM 7: …and a tool whose shape is TESSELLATED says so here.
        // Mutually exclusive with WantsAngleField by construction — no tool has both
        // a bearing and a side count, and the assert in NumericFields would be the
        // place to find out if one ever did.
        virtual bool WantsSidesField() const { return false; }

        int NumericFields( const kiwiNumField_t **out ) const override
        {
            if ( WantsAngleField() )
            {
                *out = KCON_FIELDS_LEN_ANG;
                return 2;
            }
            if ( WantsSidesField() )
            {
                *out = KCON_FIELDS_LEN_SIDES;
                return 2;
            }
            *out = KCON_FIELDS_LEN;
            return 1;
        }

        // ROUND AF, ITEM 7: the live side count, for the §13b bubble and the field
        // readout.  0 means AUTO and the tool reports what AUTO would pick, so the
        // number in the box is always the number the shape will actually have.
        virtual int ToolSides() const { return m_sidesOverride; }

        bool NumericFieldValue( int field, float *out ) const override
        {
            if ( !out )
                return false;
            // ROUND AF, ITEM 7: the side count exists BEFORE any point is placed —
            // it is a property of the tool, not of the segment being dragged — so it
            // is answered above the guard the length field needs.
            if ( field == 1 && WantsSidesField() )
            {
                const int n = ToolSides();
                if ( n <= 0 )
                    return false;                 // AUTO and nothing to report yet
                *out = (float)n;
                return true;
            }
            if ( !m_haveCur || m_pts.empty() )
                return false;
            float a[3], d[3];
            LastPoint( a );
            Sub3( m_cur, a, d );
            if ( field == 0 )
            {
                *out = Len3( d );                         // raw world units, 3D
                return true;
            }
            if ( field == 1 && WantsAngleField() )
            {
                // SHAKEOUT H: the bearing is the IN-PLANE one, and it is OMITTED
                // when the segment is mostly out of plane — under the Z lock the
                // segment IS the plane normal, and reporting "0 deg" for it would
                // be a number that means nothing and that typing into would swing
                // the segment somewhere the user did not ask for.
                const float du = Dot3( d, m_plane.u );
                const float dv = Dot3( d, m_plane.v );
                const float dn = Dot3( d, m_plane.normal );
                const float inPlane = sqrtf( du * du + dv * dv );
                if ( inPlane < 1.0e-4f || fabsf( dn ) > inPlane )
                    return false;                         // no meaningful bearing
                *out = atan2f( dv, du ) * 57.29577951308232f;
                return true;
            }
            return false;
        }

        bool BubbleAnchor( float *out3 ) const override
        {
            if ( !out3 || !m_haveCur )
                return false;
            // The MOVING END of the segment — where Plasticity pins it.
            Copy3( m_cur, out3 );
            return true;
        }

        void NumericFieldChanged( int field, bool has, float world ) override
        {
            if ( field == 1 && WantsAngleField() )
            {
                m_hasAngle = has;
                // Degrees are NOT a length: undo the numeric layer's inches→world
                // conversion to recover exactly what was typed, the same thing
                // kiwi_transform.cpp's R does (kiwi_numeric.h "KIND IS A DISPLAY
                // FACT" — the conversion is unconditional by design).
                m_angleDeg = Units_ToDisplay( world );
                ApplyAngleOverride();
                Recompute();
                g_nUpdateBits |= 1;
                return;
            }
            // ROUND AF, ITEM 7.  A COUNT is not a length: undo the numeric layer's
            // inches->world conversion to recover exactly what was typed — the same
            // rule kiwi_primitive.cpp:232-252 states for its own side field.
            if ( field == 1 && WantsSidesField() )
            {
                if ( has )
                {
                    int n = (int)floorf( Units_ToDisplay( world ) + 0.5f );
                    if ( n < KCON_SIDES_MIN ) n = KCON_SIDES_MIN;
                    if ( n > KCON_SEGS_MAX  ) n = KCON_SEGS_MAX;
                    m_sidesOverride = n;
                    KiwiCon_SetToolSides( n );    // remembered for the next gesture
                }
                else
                {
                    m_sidesOverride = 0;          // cleared: back to AUTO
                }
                Recompute();
                g_nUpdateBits |= 1;
                return;
            }
            KiwiEditorCommand::NumericFieldChanged( field, has, world );
        }

        bool Begin() override
        {
            // ROUND AF, ITEM 7: seed the count from the remembered one, so "48-sided
            // circles" is a decision the mapper makes once per session rather than
            // once per circle.  0 (never set, or explicitly cleared) is AUTO.
            m_sidesOverride = WantsSidesField() ? KiwiCon_ToolSides() : 0;
            m_pts.clear();
            m_haveCur   = false;
            m_hasNum    = false;
            m_numWorld  = 0.0f;
            m_hasAngle  = false;
            m_angleDeg  = 0.0f;
            m_zLock     = false;
            m_lastClickMs = 0;
            m_lastClickX  = -9999;
            m_lastClickY  = -9999;
            m_hud[0]    = '\0';

            KiwiCon_AutoPlaneForTool();
            m_plane     = KiwiCon_ActivePlane();
            // SHAKEOUT H FIX: remember the plane THIS GESTURE STARTED FROM, so
            // Leave() can put it back.  Latched AFTER AutoPlaneForTool on purpose:
            // deriving the plane from the face (or from the construction object)
            // under the cursor is a DELIBERATE, documented §16 change to the active
            // plane and has persisted since shakeout F.  What must NOT persist is
            // the per-point re-seat PushPoint does inside the gesture — see the
            // note there.
            m_planeOnEntry = m_plane;
            s_activeTool = this;

            // Latch a first cursor point so the very first frame already draws.
            LatchFromCursor();
            UpdateHud();
            Sys_Printf( "%s: %s\n", Name(), BeginHint() );
            return true;
        }

        // ROUND P: the console line each tool opens with.  A virtual rather than a
        // literal because the CHAINED CURVE's grammar is genuinely different from
        // the fixed-arity shapes' (Esc removes a point there, it cancels here), and
        // an opening line that describes the wrong grammar is worse than none.
        virtual const char *BeginHint() const
        { return "click to place points, Esc cancels."; }

        // SHAKEOUT H: true for the tools whose SHAPE is planar by definition — a
        // rect, a circle, an arc, an n-gon.  Those still place on the working plane
        // (a rect with one corner 40 units off its own plane is not a rect).  The
        // free-3D tools — line, polyline, spline — return false and place wherever
        // the snap says.
        virtual bool PlanarOnly() const { return true; }

        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick;
            m_snap = snap;
            if ( snap.valid )
            {
                // ── SHAKEOUT H: THE POINT IS THE SNAP'S OWN WORLD POSITION ──────
                // It used to be KiwiCon_WorldToPlane( m_plane, snap.position, … ),
                // i.e. the snapped point PROJECTED onto the working plane, which is
                // exactly the corner-snap bug: aim at a brush corner 64 units up and
                // the stored point landed at that corner's shadow on the plane.  The
                // marker drew in one place and the geometry went to another.
                //
                // Now the world position is taken verbatim.  Every arm of the snap
                // query already answers in world space — a geometry snap answers AT
                // the vertex / midpoint / crossing / face centre, and the CPLANE and
                // ANGLE arms answer on the working plane anyway, so the fallback
                // behaviour over empty space is unchanged.  (This is Plasticity's own
                // rule: PointSnap.project returns the stored point verbatim and the
                // construction plane is the lowest-priority candidate.)
                Copy3( snap.position, m_cur );
                m_haveCur = true;

                // …except for the intrinsically planar shapes, which still project.
                if ( PlanarOnly() )
                    ProjectCurOntoPlane();
                // ═════════════════════════════════════════════════════════════
                //  KIWI-UX (ROUND AA, ITEMS 3b + 6) — THE CHAIN LATCHES ITS PLANE
                // ═════════════════════════════════════════════════════════════
                // USER REPORT, verbatim: "Also it's not registering as a square
                // when floating it off the brush."  (Screenshot: a rectangle drawn
                // as a chain, its lower edge lying on the ground and its upper part
                // held against a wall, with part of it overhanging the brush's
                // edge.)  And, the same round: "line plane recognition could use
                // some more work.  Seems iffy."
                //
                // THE MECHANISM, and it is one line above this one.  Shakeout H
                // made the free-3D tools take `snap.position` VERBATIM, which was
                // the right fix for ITS bug (aim at a corner 64 units up and the
                // point used to land at that corner's shadow on the plane).  But
                // "verbatim" has no plane in it at all: as the cursor crosses from
                // the brush face to the ground, successive points come off
                // DIFFERENT surfaces, so a ring drawn part-on and part-off a brush
                // is not one polygon — it is four points that do not share a plane.
                // Some rungs land within a hair of coplanar and some do not, which
                // is exactly the "seems iffy" report: it worked when the surfaces
                // happened to agree and silently did not when they did not.
                //
                // kiwi_region.cpp will not accept a non-planar ring (KVALID_PLANE_DOT
                // and the coplanarity tests it runs before an arrangement), and it
                // is RIGHT not to — a skew quadrilateral is not a face.  So the
                // shape "did not register as a square".
                //
                // THE FIX IS AT THE SOURCE, NOT AT THE ACCEPTANCE TEST.  Loosening
                // the region tolerance would take a genuinely skew ring and pretend
                // it was flat; every consumer downstream (the extrude prism, the
                // brush the region becomes) would then be built off a plane that
                // does not contain its own outline.  Instead the chain LATCHES:
                //
                //   point 1  free.  It may come from anywhere — a corner 64 units
                //            up, a face centre, the grid — and PushPoint re-seats
                //            the working plane's ORIGIN through it, so the plane
                //            now passes through where the user started.
                //   point 2+ ON THAT PLANE.  The snap still chooses WHERE in the
                //            plane (its u,v are taken from the snapped position),
                //            it just cannot leave it.
                //
                // So a closed ring is coplanar BY CONSTRUCTION and the region test
                // can stay strict.  This is also what makes the plane PREDICTABLE
                // (item 6): within one gesture there is exactly one plane, chosen
                // once, and no later rung can change it under the user.
                //
                // THE ESCAPE IS THE Z LOCK, unchanged and deliberately the only
                // one.  ApplyZLock runs immediately below and outranks this — it is
                // an explicit constraint the user turned on, and it is how a
                // genuinely non-planar polyline is drawn.  The cost is recorded in
                // RADIANT_KNOWN_ISSUES: a free 3D polyline now needs Z per rising
                // segment, where before it came out of wherever the snaps landed.
                else if ( PointCount() >= 1 )
                {
                    ProjectCurOntoPlane();
                }
            }
            // SHAKEOUT H: the Z lock outranks the snap — it is an explicit
            // constraint the user turned on, and a constraint that a snap could
            // silently overrule is not a constraint.  A GEOMETRY snap still
            // contributes its HEIGHT (the vertical line is where the point goes,
            // the snap says how far up it), which is what makes "start here, go up
            // to that corner's level" work; anything else uses the cursor ray.
            ApplyZLock();
            // KIWI-UX (shakeout E): a typed BEARING outranks the cursor's, and it
            // has to be re-applied here — the snap above has just overwritten
            // m_cur from the pointer.  Applied BEFORE Recompute so a tool that
            // derives its length from m_cur's direction (the line tool) sees the
            // typed direction, i.e. angle and length compose instead of fighting.
            ApplyAngleOverride();
            Recompute();
            g_nUpdateBits |= 1;
        }

        void NumericChanged( bool has, float world ) override
        {
            m_hasNum   = has && KiwiNum_HasValue();
            m_numWorld = world;
            ApplyAngleOverride();
            Recompute();
        }

        bool KeyDown( int vk, unsigned int mods ) override
        {
            // SHAKEOUT I: Ctrl+Z inside a construction tool is now THE SAME Ctrl+Z
            // as everywhere else — the unified journal (kiwi_undo.h), which pops
            // whichever step is genuinely newest rather than assuming that being
            // inside a drawing tool means the newest step was a drawing one.  That
            // assumption is exactly what ruling 2's "split undo" note predicted
            // would surprise a user.  Consuming the key here still keeps the
            // legacy Ctrl+Z in Radiant_PreTranslateMessage from ALSO firing (the
            // funnel runs first), so the key is handled exactly once.
            // Ctrl+Shift+Z is the redo, matching the Ctrl+Y the accelerator path
            // carries — inside a tool there is no accelerator to reach.
            if ( vk == 0x5A && ( mods & 4 ) != 0 )      // Ctrl+Z / Ctrl+Shift+Z
            {
                if ( ( mods & 1 ) != 0 )
                {
                    if ( !KiwiUndo_Redo() )
                        Sys_Printf( "Nothing left to redo.\n" );
                }
                else if ( !KiwiUndo_Undo() )
                {
                    Sys_Printf( "Nothing left to undo.\n" );
                }
                return true;
            }
            if ( vk == 0x59 && ( mods & 4 ) != 0 )      // Ctrl+Y
            {
                if ( !KiwiUndo_Redo() )
                    Sys_Printf( "Nothing left to redo.\n" );
                return true;
            }

            // ── SHAKEOUT H: Z — THE VERTICAL CONSTRAINT ─────────────────────────
            // USER REPORT: "the lines are only in 2D.  It's not possible, even with
            // an aggressive camera angle, to get them to go up or down on Z."  A
            // free-3D placement (see MouseMove) fixes the STORE; this fixes the
            // AIMING, because hitting an exact height by eye through a perspective
            // camera is not a thing anyone can do.
            //
            // A TOGGLE, NOT A HOLD, and the reason is mechanical rather than a
            // preference: the funnel this key arrives through
            // (Radiant_PreTranslateMessage -> KiwiUX_KeyFunnel) is fed WM_KEYDOWN
            // only — there is no key-UP path into a command anywhere in this layer,
            // so a hold-to-constrain would latch on and never let go.  The HUD says
            // "Z: vertical ON" whenever it is engaged, which is the visible state a
            // sticky mode owes the user (§13's HUD rule).
            //
            // THE MATCH-FACE ARBITRATION (bare Z is Match Face, kiwi_keymap.h).
            // There is no conflict and no new rule: KiwiUX_KeyFunnel's FIRST arm is
            // `if ( g_activeCommand ) return KiwiCmd_KeyDown( vk, mods );`
            // (kiwi_command.cpp:983-984), so with a drawing tool live the key never
            // reaches Radiant_TryHotkey and Match Face cannot fire.  With no command
            // live this code does not exist and Z is Match Face exactly as shakeout
            // G left it.  Match Face is a SELECTION-context verb and a drawing tool
            // is a COMMAND context; the funnel already ranks them.
            //
            // This is also what Plasticity does — its axis choices live in the
            // `body[gizmo=point-picker]` keymap scope (default-keymap.ts:353-366,
            // "z": "snaps:set-z"), i.e. bound to the POINT PICKER rather than
            // globally, so they shadow the global bindings only while it runs.
            //
            // REFUSED on the intrinsically planar tools: a rect / circle / arc /
            // n-gon only exists in a plane, so "put this corner 128 units above the
            // plane" is not a thing the shape can mean.  Consumed anyway, so the key
            // cannot fall through to Match Face mid-gesture.
            if ( vk == 0x5A && !mods )                  // Z
            {
                if ( PlanarOnly() )
                {
                    Sys_Printf( "%s: this shape is planar — Z does nothing here "
                                "(use the line, polyline or spline tool).\n", Name() );
                    return true;
                }
                m_zLock = !m_zLock;
                if ( m_zLock && m_pts.empty() )
                {
                    m_zLock = false;                    // nothing to go up FROM yet
                    Sys_Printf( "%s: place a point first — Z constrains the NEXT segment.\n",
                                Name() );
                }
                else
                {
                    Sys_Printf( "%s: vertical constraint %s.\n",
                                Name(), m_zLock ? "ON" : "off" );
                }
                UpdateHud();
                g_nUpdateBits |= 1;
                return true;
            }

            // Esc ladder (§7 item 22): the FIRST press drops the in-progress chain,
            // the second leaves the tool.  Reachable because the framework offers
            // Esc to the command before acting on it (kiwi_command.h).
            if ( vk == 0x1B && !m_pts.empty() )         // VK_ESCAPE
            {
                m_pts.clear();
                m_zLock = false;
                // SHAKEOUT H FIX: a tool with PER-STAGE state of its own has to hear
                // about this.  The line tool's parked-endpoint latch survived the
                // clear, so Esc-then-click left the rubber band frozen at the old
                // parked point and RMB then committed a zero-length line.  One hook
                // rather than a base-class member, because the state is the LINE's
                // and no other tool has any.
                OnChainCleared();
                UpdateHud();
                g_nUpdateBits |= 1;
                Sys_Printf( "%s: chain cleared (Esc again to exit).\n", Name() );
                return true;
            }

            // Enter ENDS a multi-point tool that already has enough points; for the
            // fixed-arity tools the framework's Enter-commits is right as-is.
            if ( vk == 0x0D && WantsEnterFinish() )     // VK_RETURN
            {
                Finish( false );
                return true;
            }
            return false;
        }

        void Commit() override
        {
            // Enter / an ending click arrive here.  Finish() is idempotent: a tool
            // that already stored its object on the ending click stores nothing.
            Finish( false );
            Leave();
        }

        void Cancel() override
        {
            m_pts.clear();
            Leave();
            g_nUpdateBits |= 1;
        }

        void DrawWorld() override
        {
            DrawWorkingPlane();
            DrawChain();
        }

        // Framework click hook: place a point.  Returning false COMMITS.
        bool Click() override
        {
            if ( !m_haveCur )
                return true;

            int cx = 0, cy = 0;
            const bool haveCur = KiwiCmd_LastCursor( &cx, &cy );
            const unsigned nowMs = (unsigned)::GetTickCount();
            const bool dbl = haveCur
                          && ( nowMs - m_lastClickMs ) <= 400u
                          && abs( cx - m_lastClickX ) <= 4
                          && abs( cy - m_lastClickY ) <= 4;
            m_lastClickMs = nowMs;
            m_lastClickX  = cx;
            m_lastClickY  = cy;

            return OnClick( dbl );
        }

        // ── what each tool must answer ──────────────────────────────────────
        virtual bool OnClick( bool doubleClick ) = 0;
        virtual void Recompute() = 0;
        virtual void UpdateHud() = 0;
        virtual bool WantsEnterFinish() const { return false; }
        // Store whatever the tool has, if anything is storable.  `fromEnter` is
        // informational (a polyline ended by Enter stays open).
        virtual void Finish( bool fromEnter ) { (void)fromEnter; }
        // SHAKEOUT H FIX: the Esc rung has just dropped the in-progress chain.  A
        // tool holding per-stage state of its own resets it here.
        virtual void OnChainCleared() {}

        // ── shared helpers ──────────────────────────────────────────────────
        bool LatchFromCursor()
        {
            ray_t ray;
            if ( !Pick_RayFromCursor( &ray ) )
                return false;
            float w[3];
            if ( !KiwiCon_RayPlaneBounded( m_plane, ray, w ) )
                return false;
            float uv[2];
            KiwiCon_WorldToPlane( m_plane, w, uv );
            KiwiCon_SnapUV( m_plane, uv );        // ROUND R: world-anchored lattice
            KiwiCon_PlaneToWorld( m_plane, uv, m_cur );
            m_haveCur = true;
            return true;
        }

        void Leave()
        {
            if ( s_activeTool == this )
                s_activeTool = 0;
            // ── SHAKEOUT H FIX: PUT THE GLOBAL ACTIVE PLANE BACK ────────────────
            // PushPoint re-seats it at every placed point (see the note there), and
            // nothing used to undo that.  The consequences were both permanent:
            // draw one line up at z = 128 and EVERY later tool started on a plane at
            // z = 128 — so the next tool's first point landed 128 units off the
            // ground with nothing on screen saying why.  (Shakeout H also noticed it
            // through the cplane grid patch, which never went away again; ROUND M
            // deleted that patch, but the leak it exposed is real and this is still
            // the fix for it.)
            //
            // The re-seat INSIDE the gesture stays: it is what makes the second
            // point of a rising segment fall back to the height being worked at.
            // Only its escape is closed.
            KiwiCon_SetActivePlane( m_planeOnEntry );
            m_pts.clear();
            m_haveCur = false;
            m_zLock   = false;
            m_hud[0]  = '\0';
            OnChainCleared();
        }

        // ── SHAKEOUT H: plane-space convenience for the PLANAR tools ────────
        // The rect / circle / arc / polygon tools are still solved in (u,v) — their
        // shapes only mean anything in a plane — so they convert at the edges.
        void CurUV( float uv[2] ) const   { KiwiCon_WorldToPlane( m_plane, m_cur, uv ); }
        void SetCurUV( const float uv[2] ){ KiwiCon_PlaneToWorld( m_plane, uv, m_cur ); }
        void LastPointUV( float uv[2] ) const
        {
            float w[3];
            LastPoint( w );
            KiwiCon_WorldToPlane( m_plane, w, uv );
        }
        void ProjectCurOntoPlane()
        {
            float uv[2];
            CurUV( uv );
            SetCurUV( uv );
        }

        // ── SHAKEOUT H: the vertical constraint (see the Z rung in KeyDown) ─
        // The placement is the CLOSEST POINT ON THE VERTICAL LINE through the last
        // placed point to the cursor ray — the same "closest point between two
        // lines" solve the trim tool uses, run against a ray long enough to cover
        // the world.  With a GEOMETRY snap live the snap's own height is taken
        // instead, so "go straight up to that corner's level" is one gesture.
        void ApplyZLock()
        {
            if ( !m_zLock || m_pts.empty() )
                return;
            float a[3];
            LastPoint( a );

            if ( m_snap.valid && KiwiSnap_IsGeometry( m_snap.type ) )
            {
                m_cur[0] = a[0];
                m_cur[1] = a[1];
                m_cur[2] = m_snap.position[2];
                m_haveCur = true;
                return;
            }

            ray_t ray;
            if ( !Pick_RayFromCursor( &ray ) )
                return;
            const float rayEnd[3] = { ray.origin[0] + ray.dir[0] * 1.0e5f,
                                      ray.origin[1] + ray.dir[1] * 1.0e5f,
                                      ray.origin[2] + ray.dir[2] * 1.0e5f };
            // A generous vertical span both ways so the closest-approach solve is
            // never clamped by the segment's own ends.
            const float vlo[3] = { a[0], a[1], a[2] - 1.0e5f };
            const float vhi[3] = { a[0], a[1], a[2] + 1.0e5f };
            float t = 0.0f;
            KiwiCon_SegSegClosest( vlo, vhi, ray.origin, rayEnd, &t, 0, 0 );
            float z = vlo[2] + ( vhi[2] - vlo[2] ) * t;
            const float s = KiwiUnits_GridSpacingWorld();
            if ( s > 0.0f )
                z = floorf( z / s + 0.5f ) * s;
            m_cur[0] = a[0];
            m_cur[1] = a[1];
            m_cur[2] = z;
            m_haveCur = true;
        }

        // KIWI-UX (shakeout E): swing the current segment onto a TYPED BEARING,
        // keeping its length.  The exact inverse of the 15° angle lock the snap
        // already applies (kiwi_snap.cpp arm 7) — quantise the direction, keep
        // the distance — which is why it composes with everything downstream:
        // a tool that then applies a typed LENGTH reads the direction back out of
        // m_cur and gets the one the user asked for.
        //
        // SHAKEOUT H: the swing is now the IN-PLANE part only and the out-of-plane
        // offset is KEPT, so a typed bearing on a segment that also rises does what
        // it says (turn it) instead of flattening it.  Suppressed under the Z lock,
        // where there is no bearing to swing.
        void ApplyAngleOverride()
        {
            if ( !m_hasAngle || !m_haveCur || m_pts.empty() || m_zLock )
                return;
            float a[3], d[3];
            LastPoint( a );
            Sub3( m_cur, a, d );
            const float du = Dot3( d, m_plane.u );
            const float dv = Dot3( d, m_plane.v );
            const float dn = Dot3( d, m_plane.normal );
            const float len = sqrtf( du * du + dv * dv );
            if ( !( len > 1.0e-4f ) )
                return;                       // no in-plane length: nothing to swing
            const float rad = m_angleDeg * 0.01745329252f;
            const float nu  = cosf( rad ) * len;
            const float nv  = sinf( rad ) * len;
            for ( int k = 0; k < 3; ++k )
                m_cur[k] = a[k] + m_plane.u[k] * nu + m_plane.v[k] * nv
                                + m_plane.normal[k] * dn;
        }

        // SHAKEOUT H: placing a point RE-SEATS THE WORKING PLANE THROUGH IT.  The
        // fallback plane (what ray∩plane answers over empty space, and what the
        // grid patch draws) has to follow the height the user is actually working
        // at, or the second point of a segment that went up 128 units would fall
        // back to the ORIGINAL height the moment the cursor left a snap target.
        // Pushed to the ACTIVE plane as well, because the snap layer reads
        // KiwiCon_ActivePlane() and not this tool's copy.
        void PushPoint( const float w[3] )
        {
            // KIWI-UX (ROUND AA, ITEM 6): THE RE-SEAT IS THE FIRST POINT'S ONLY.
            // With the chain latch above in force, every point after the first is
            // already ON m_plane, so re-seating the origin through it is a no-op
            // by construction — writing it anyway would only be a chance for float
            // drift to walk the plane one epsilon per point across a long chain,
            // which is the same class of defect as the one the latch just fixed.
            // Doing it on the first point is what shakeout H's note is about and
            // is still exactly right: it puts the fallback plane (what ray-plane
            // answers over empty space) at the height the user is working at.
            const bool first = m_pts.empty();
            m_pts.push_back( w[0] );
            m_pts.push_back( w[1] );
            m_pts.push_back( w[2] );
            if ( first )
            {
                Copy3( w, m_plane.origin );
                KiwiCon_SetActivePlane( m_plane );
            }
        }

        int PointCount() const { return (int)( m_pts.size() / 3 ); }

        void LastPoint( float out[3] ) const
        {
            Copy3( &m_pts[m_pts.size() - 3], out );
        }

        void FirstPoint( float out[3] ) const
        {
            Copy3( &m_pts[0], out );
        }

        // "Did the user click near the first point?" — in PIXELS, never world
        // units, for the same reason every pick tolerance in this layer is
        // (kiwi_pick.h): a world tolerance breaks at distance.
        bool NearFirstPoint() const
        {
            if ( PointCount() < 3 )
                return false;
            float fw[3];
            FirstPoint( fw );
            float fx, fy, cx2, cy2;
            if ( !Pick_WorldToImage( fw, &fx, &fy ) || !Pick_WorldToImage( m_cur, &cx2, &cy2 ) )
                return false;
            const float dx = fx - cx2, dy = fy - cy2;
            return sqrtf( dx * dx + dy * dy ) <= KCON_JOIN_PIXELS;
        }

        void SetHud( const char *fmt, ... )
        {
            va_list ap;
            va_start( ap, fmt );
            _vsnprintf( m_hud, sizeof( m_hud ), fmt, ap );
            va_end( ap );
            m_hud[sizeof( m_hud ) - 1] = '\0';
        }

        // ═════════════════════════════════════════════════════════════════════
        //  KIWI-UX (ROUND AA, ITEM 6.3) — SHOW THE PLANE YOU ARE DRAWING ON
        // ═════════════════════════════════════════════════════════════════════
        // USER REPORT, verbatim: "line plane recognition could use some more work.
        // Seems iffy."
        //
        // Half of "iffy" is that the plane was only ever announced in the CONSOLE
        // ("Construction plane: from face under cursor.") — a line that scrolls
        // away, in a pane the user is not looking at while drawing.  Round M
        // deleted the old cplane patch for good reasons (see the note further
        // down this file), and every one of them still holds: it was a permanent
        // fixture that fought §17's ground grid and drew for commands that were
        // not placing anything.  This is not that patch coming back.  It is
        // BOUNDED, it is only alive while a drawing tool is running, and it is
        // sized in PIXELS at the anchor so it reads the same at any zoom.
        //
        // ── WHAT PLASTICITY SHOWS, since it is worth being close to ──────────
        // A two-tier LINE grid, never a fill: GridHelper.getOverlay
        // (GridHelper.ts:19-36) positions the grid at `constructionPlane.p` and
        // orients it by `constructionPlane.orientation`, and the grid itself is
        // two stacked THREE.GridHelpers — fine lines plus heavy lines at
        // divisions/10 (FloorHelper.ts:90, sizes at GridHelper.ts:6-15).
        // Crucially its opacity is the SQUARED grazing angle,
        //     const dot = grid.dot( eye );  material.opacity = dot * dot;
        // (FloorHelper.ts:124-135), so the plane fades to nothing as you look
        // along it rather than turning into a wall of aliased lines.  That fade is
        // reproduced here — it is the part that makes a plane indicator readable
        // instead of noisy — as a COLOUR ramp rather than an alpha, because
        // kiwi_lines.h TRAP 2 is explicit that alpha on this path means "blend
        // less of my colour in", not "be transparent".  Their grid is centred on
        // the PLANE's origin and never on the cursor (there is no cursor-follow
        // anywhere in GridHelper.ts), which is what this does too: the anchor is
        // the chain's first point once one exists, and the plane origin before.
        void DrawWorkingPlane()
        {
            const camera_s *c = Ed_Camera();
            if ( !c )
                return;

            // The grazing fade, Plasticity's dot² (FloorHelper.ts:124-135).  Below
            // the floor there is nothing worth drawing and the lines would be a
            // single bright smear along one screen row.
            const float d   = Dot3( m_plane.normal, c->vpn );
            const float fade = d * d;
            if ( fade < 0.06f )
                return;

            // The anchor: where the work IS.  The chain's first point once the
            // chain has one (that is the point the plane was seated through — see
            // PushPoint), the plane origin before that.
            float o[3];
            if ( PointCount() >= 1 )
                FirstPoint( o );
            else
                Copy3( m_plane.origin, o );

            // Sized in PIXELS at the anchor, like every other accent in this
            // layer, so the indicator is the same size on screen at any zoom and
            // cannot swallow the map when the camera is far away.
            const float perPx = KiwiCam_WorldPerPixel( o );
            if ( !( perPx > 0.0f ) )
                return;
            const float half = perPx * KCON_PLANE_HALF_PIXELS;

            // The colour: the active-chain amber, dimmed by the grazing fade.  A
            // DIMMER COLOUR and not a lower alpha — kiwi_lines.h TRAP 2.
            const float k = 0.35f + 0.45f * fade;
            KiwiLines_Color( KCON_COL_ACTIVE[0] * k,
                             KCON_COL_ACTIVE[1] * k,
                             KCON_COL_ACTIVE[2] * k );

            // A bounded square outline plus one cross through the anchor: enough
            // to read the plane's ORIENTATION and its HEIGHT at a glance, which is
            // the whole question ("am I about to draw on the wall or on the
            // floor?"), without a grid's segment count.  KiwiLines_Add refuses
            // past the open batch's budget, so this cannot starve the chain that
            // draws after it (kiwi_lines.h TRAP 1) — and it is drawn FIRST on
            // purpose so that if anything is dropped it is this and not the
            // geometry the user is actually placing.
            float corner[4][3];
            for ( int i = 0; i < 4; ++i )
            {
                const float su = ( i == 0 || i == 3 ) ? -half : half;
                const float sv = ( i < 2 )            ? -half : half;
                for ( int kk = 0; kk < 3; ++kk )
                    corner[i][kk] = o[kk] + m_plane.u[kk] * su + m_plane.v[kk] * sv;
            }
            for ( int i = 0; i < 4; ++i )
                if ( !KiwiLines_Add( corner[i], corner[( i + 1 ) % 4] ) )
                    return;

            const float cross = half * 0.18f;
            float a[3], b[3];
            for ( int kk = 0; kk < 3; ++kk ) { a[kk] = o[kk] - m_plane.u[kk] * cross;
                                               b[kk] = o[kk] + m_plane.u[kk] * cross; }
            if ( !KiwiLines_Add( a, b ) )
                return;
            for ( int kk = 0; kk < 3; ++kk ) { a[kk] = o[kk] - m_plane.v[kk] * cross;
                                               b[kk] = o[kk] + m_plane.v[kk] * cross; }
            KiwiLines_Add( a, b );
        }

        // The rubber band + already-placed chain, inside the framework's batch.
        void DrawChain()
        {
            if ( m_pts.empty() && !m_haveCur )
                return;
            KiwiLines_Color( KCON_COL_ACTIVE[0], KCON_COL_ACTIVE[1], KCON_COL_ACTIVE[2] );
            const int n = PointCount();
            for ( int i = 0; i + 1 < n; ++i )
                if ( !KiwiLines_Add( &m_pts[(size_t)i * 3], &m_pts[( (size_t)i + 1 ) * 3] ) )
                    return;
            if ( n >= 1 && m_haveCur )
                KiwiLines_Add( &m_pts[( (size_t)n - 1 ) * 3], m_cur );
        }

        kconPlane_t        m_plane;
        // SHAKEOUT H FIX: the ACTIVE plane as this gesture found it, restored by
        // Leave().  See the notes on Begin() and Leave().
        kconPlane_t        m_planeOnEntry;
        std::vector<float> m_pts;                 // placed points, 3 floats, WORLD
        float              m_cur[3] = { 0.0f, 0.0f, 0.0f };
        bool               m_haveCur = false;
        bool               m_zLock   = false;     // shakeout H: the Z constraint
        bool               m_hasNum  = false;
        int                m_sidesOverride = 0;   // ROUND AF, ITEM 7 — 0 = automatic
        float              m_numWorld = 0.0f;
        // shakeout E: the typed BEARING (field 1), degrees in the plane's own
        // (u,v) basis — the same basis the 15° angle lock measures in.
        bool               m_hasAngle = false;
        float              m_angleDeg = 0.0f;
        snap_result_t      m_snap;
        unsigned           m_lastClickMs = 0;
        int                m_lastClickX  = -9999;
        int                m_lastClickY  = -9999;
        char               m_hud[192] = { 0 };
    };

    // ═════════════════════════════════════════════════════════════════════════
    //  ROUND P — THE CHAINED CURVE.  LINE AND POLYLINE ARE ONE TOOL NOW.
    // ═════════════════════════════════════════════════════════════════════════
    //
    // USER DIRECTIVE, verbatim: "You need to allow chain-lines being created.  I
    // should be able to make a square or more advanced shape without opening the
    // line tool again.  blam, blam, blam, blam, click it out and it should be 1
    // line when finished.  (Maybe the entire line concept is called a 'curve' in
    // plasticity, look it up).  We need this throughout the entire system.  Too
    // slow to do A->B over and over again."
    //
    // ── LOOKED IT UP, AND THE GUESS IS EXACTLY RIGHT ────────────────────────
    // In Plasticity "line" IS "curve" — LineCommand is a THREE-LINE subclass of
    // CurveCommand that changes only the c3d curve type and the keyboard gizmo:
    //     export class LineCommand extends CurveCommand {
    //         protected type = c3d.SpaceType.Polyline3D;
    //         protected get keyboard() { return new LineKeyboardGizmo(this.editor); };
    //     }                                        (CurveCommand.ts:68-71)
    // and shift-a — the key this editor already uses for it — binds `command:line`
    // (default-keymap.ts:283-284).  The chaining is a `while (true)` around ONE
    // point picker (CurveCommand.ts:40-60): each iteration awaits one point, pushes
    // it and updates the curve; `point-picker:finish` — bound to BOTH `mouse2` and
    // `enter` (default-keymap.ts:353-363) — rejects with a Finish exception, which
    // breaks the loop and commits ONE curve (CurveCommand.ts:56-64).  Closing is a
    // named PointSnap on the start point, added once there are >= 3 points
    // (CurveCommand.ts:73-81), plus `wouldBeClosed` (CurveFactory.ts:114-116).
    // Removing the last point is `gizmo:line:undo` -> pointPicker.undo() +
    // makeCurve.undo() (CurveCommand.ts:31-36).
    //
    // ── SO THE TWO TOOLS COLLAPSE INTO ONE ──────────────────────────────────
    //   click            place a point; the rubber band runs to the next
    //   click near the
    //     FIRST point    close the loop and finish        (needs >= 3 points)
    //   double-click     finish                            (needs >= 2 points)
    //   RMB / Enter      FINISH THE WHOLE CHAIN as ONE object
    //   Esc              REMOVE THE LAST POINT; an empty chain leaves the tool
    //   Z                the vertical constraint (shakeout H) — still here, and
    //                    still the second way to do what ROUND P's axis guides
    //                    (kiwi_snap.h) now do by aiming
    //
    // BOTH COMMAND IDS RUN THIS ONE TOOL (KiwiCon_CommandForId): "Line" and
    // "Polyline" in the Shift+A menu and in the palette are the same command now.
    // They are NOT unregistered — a keymap, a menu row or a user macro naming
    // KiwiConstructPolyline must keep working — they simply resolve to the same
    // object, which is the honest way to merge two commands that no longer differ.
    //
    // ── WHAT THIS DELIBERATELY DROPS: shakeout H's PARKED ENDPOINT ──────────
    // The old two-click line let click 2 PARK the endpoint (and click 3 resume it)
    // so the user could sweep the cursor away to line something up.  That grammar
    // and this one cannot coexist: click 2 cannot both park the end and place the
    // second point.  The NEED behind the shakeout-H directive — "lines should not
    // confirm until a right-click or Enter" — is still met, and more directly than
    // before: NO click commits anything any more, so the user can keep placing and
    // re-aiming for as long as they like and only RMB/Enter ends the object.  What
    // is gone is the ability to re-aim a point ALREADY PLACED; the replacement is
    // Esc, which removes it so it can be placed again.  Logged in
    // RADIANT_KNOWN_ISSUES.md.
    //
    // ── THE OBJECT IT STORES ────────────────────────────────────────────────
    // ONE kconObject_t for the whole chain: KCON_LINE when it came out at exactly
    // two points, KCON_POLYLINE otherwise.  That is not a new rule — it is the
    // convention every other producer in the tree already follows (kiwi_trim.cpp:558,
    // kiwi_offset.cpp:577, kiwi_conselect.cpp:826), and the consumers treat the two
    // types identically (kiwi_trim.cpp:81 accepts both; the region/arrangement,
    // join, offset and extrude paths all walk KiwiCon_SegmentCount /
    // KiwiCon_VertWorld, which are type-agnostic).
    class KiwiCurveTool : public KiwiDrawTool
    {
    public:
        const char *Name() const override { return "Construct Curve"; }
        // shakeout E: Tab cycles length → angle here (each SEGMENT is a bearing).
        bool WantsAngleField() const override { return true; }
        // shakeout H: a curve is not a planar shape — it places wherever the snap is.
        bool PlanarOnly() const override { return false; }

        // DELIBERATELY the base's `false`.  With it false, Enter falls through this
        // tool to the framework's Enter rung, which runs Commit() = Finish + Leave —
        // i.e. finish the chain AND end the command, which is exactly what
        // Plasticity's `break` out of the while-loop does (CurveCommand.ts:56-64).
        // RMB arrives at the same place: KiwiCmd_Confirm is literally
        // KiwiCmd_KeyDown(VK_RETURN) (kiwi_command.cpp:975-982), so one answer here
        // covers both halves of `point-picker:finish`.

        const char *BeginHint() const override
        {
            return "click to chain points, click the first point to close, "
                   "RMB/Enter finishes, Esc drops the last point.";
        }

        // The base advertises "Esc: Clear chain", which is no longer what Esc does.
        int HudPrompts( const kiwiPrompt_t **out ) const override
        {
            static const kiwiPrompt_t s_curve[] = {
                { "aim",  "Z guide = vertical" },
                { "Z",    "Vertical lock" },
                { "Esc",  "Drop last point" },
            };
            *out = s_curve;
            return (int)( sizeof( s_curve ) / sizeof( s_curve[0] ) );
        }

        bool Begin() override
        {
            m_wantClosed = false;
            return KiwiDrawTool::Begin();
        }

        bool KeyDown( int vk, unsigned int mods ) override
        {
            // ── Esc REMOVES THE LAST POINT ───────────────────────────────────
            // ABOVE the base class, whose Esc rung drops the WHOLE chain — which is
            // the wrong granularity for a tool whose whole point is that a chain is
            // long.  Mis-clicking the seventh point of a shape must not cost the
            // first six.  With the chain empty this falls through to the base (which
            // also refuses an empty chain) and then to the framework's cancel rung,
            // so the LAST Esc still leaves the tool.
            //
            // (Plasticity binds this to ctrl-z instead — default-keymap.ts:182-194
            // `"[command='line'] plasticity-viewport": { "ctrl-z": "gizmo:line:undo" }`.
            // That slot is not free here: shakeout I made Ctrl+Z inside a drawing
            // tool the ONE unified undo journal (see the base class's KeyDown), and
            // splitting it again is exactly what ruling 2 warned against.  Esc is
            // the slot that was free, and it was doing something worse.)
            if ( vk == 0x1B && !m_pts.empty() )         // VK_ESCAPE
            {
                m_pts.resize( m_pts.size() - 3 );
                ReseatPlane();
                if ( m_pts.empty() )
                    m_zLock = false;                    // nothing to go up FROM
                UpdateHud();
                g_nUpdateBits |= 1;
                Sys_Printf( "Curve: removed the last point (%i left%s).\n",
                            PointCount(),
                            m_pts.empty() ? ", Esc again to exit" : "" );
                return true;
            }
            return KiwiDrawTool::KeyDown( vk, mods );
        }

        bool OnClick( bool doubleClick ) override
        {
            // CLOSE: the near-first-point test is in PIXELS (NearFirstPoint), which
            // is this layer's rule everywhere.  Plasticity instead relies on the
            // named "Closed" PointSnap having actually been taken and then compares
            // world positions at 10e-6 (CurveFactory.ts:114-116) — same intent, but
            // it needs its snap system's exact-hit guarantee, which our snap ranking
            // (a brush vertex outranks a construction anchor at equal pixel
            // distance) does not offer.  The pixel test is the honest local form.
            if ( PointCount() >= 3 && NearFirstPoint() )
            {
                m_wantClosed = true;
                Finish( false );
                return false;                      // closing the loop ENDS the object
            }
            if ( doubleClick && PointCount() >= 2 )
            {
                Finish( false );
                return false;
            }
            if ( PointCount() >= KCON_MAX_POINTS )
            {
                Sys_Printf( "Curve: %i point limit reached — ending here.\n", KCON_MAX_POINTS );
                Finish( false );
                return false;
            }
            PushPoint( m_cur );
            UpdateHud();
            return true;
        }

        void Recompute() override
        {
            // A typed value is the LENGTH of the CURRENT segment along its own
            // direction (§7 item 22's numeric rule), which is now "the segment
            // leaving the last placed point" for every point in the chain.
            if ( m_hasNum && PointCount() >= 1 )
            {
                float a[3], d[3];
                LastPoint( a );
                Sub3( m_cur, a, d );
                const float l = Len3( d );
                if ( l > 1.0e-4f )
                    Mad3( a, d, m_numWorld / l, m_cur );
            }
            UpdateHud();
        }

        void UpdateHud() override
        {
            // §16b HUD PROMPTS: every stage says what the next CLICK does and what a
            // typed number would mean.  The numeric HUD line (kiwi_numeric.cpp)
            // renders this fragment between the command name and the typed buffer,
            // so the prompt is the only place a drawing tool can teach its grammar.
            const int n = PointCount();
            if ( n == 0 )
            {
                SetHud( "curve  ·  click: first point  ·  keep clicking to chain" );
                return;
            }
            float a[3], d[3];
            LastPoint( a );
            Sub3( m_cur, a, d );
            char b[32], dz[32];
            KiwiUnits_Format( b,  sizeof( b ),  Len3( d ) );
            KiwiUnits_Format( dz, sizeof( dz ), d[2] );
            // ROUND P: BOTH vertical routes are named.  Aiming at the dashed Z
            // guide (kiwi_snap.h arm 4c) is now the primary one — it needs no mode
            // and no key — and the shakeout-H Z toggle is still here for the times
            // the camera cannot give a usable angle at all.
            SetHud( "curve  %i pts  seg %s  dz %s%s  ·  %s  ·  RMB/Enter: finish  ·  "
                    "Esc: drop last  ·  vertical: aim the Z guide%s",
                    n, b, dz, m_zLock ? "  [Z LOCK]" : "",
                    ( n >= 3 && NearFirstPoint() )
                        ? "click: CLOSE the loop"
                        : "click: next point",
                    m_zLock ? ", or Z (LOCKED)" : ", or press Z" );
        }

        void Finish( bool ) override
        {
            // The point under the cursor is only taken when the chain is not being
            // closed: closing means "join back to the first point", and appending
            // the near-first cursor point as well would leave a duplicate vertex
            // that the region loop-finder would then have to dedup.
            std::vector<float> pts = m_pts;
            if ( !m_wantClosed && m_haveCur )
            {
                // …and it is only taken when it is somewhere ELSE.  RMB immediately
                // after placing a point (the "I am done, this last one was the end"
                // gesture) would otherwise append a duplicate vertex on top of it.
                float last[3], d[3];
                bool  take = true;
                if ( !m_pts.empty() )
                {
                    LastPoint( last );
                    Sub3( m_cur, last, d );
                    take = ( Len3( d ) >= 1.0e-3f );
                }
                if ( take )
                {
                    pts.push_back( m_cur[0] );
                    pts.push_back( m_cur[1] );
                    pts.push_back( m_cur[2] );
                }
            }
            if ( (int)( pts.size() / 3 ) < 2 )
            {
                // Only worth saying when the user HAD started a chain: finishing a
                // tool that was opened and immediately dismissed is not an error.
                if ( !m_pts.empty() )
                    Sys_Printf( "Curve: needs at least two points — nothing placed.\n" );
                m_pts.clear();
                m_wantClosed = false;
                return;
            }
            kconObject_t o;
            // The tree's own convention for "a chain that came out at two points"
            // (kiwi_trim.cpp:558, kiwi_offset.cpp:577, kiwi_conselect.cpp:826).
            o.type   = ( pts.size() == 6 ) ? KCON_LINE : KCON_POLYLINE;
            o.plane  = m_plane;                    // seed only; refit on Add
            o.pts    = pts;
            o.closed = m_wantClosed;
            KiwiCon_UndoPush();
            KiwiCon_Add( o );
            Sys_Printf( "Curve: %i points%s.\n", (int)( pts.size() / 3 ),
                        m_wantClosed ? ", closed" : "" );
            m_pts.clear();
            m_wantClosed = false;
        }

    private:
        // PushPoint re-seats the working plane through every placed point (see the
        // note there); popping one has to put it back, or the fallback plane keeps
        // the height of a point that no longer exists.
        void ReseatPlane()
        {
            if ( m_pts.empty() )
            {
                m_plane = m_planeOnEntry;
            }
            else
            {
                float w[3];
                LastPoint( w );
                Copy3( w, m_plane.origin );
            }
            KiwiCon_SetActivePlane( m_plane );
        }

        bool m_wantClosed = false;
    };

    // ── RECT — two corners, axis-aligned IN PLANE ───────────────────────────
    class KiwiRectTool : public KiwiDrawTool
    {
    public:
        const char *Name() const override { return "Construct Rectangle"; }

        bool OnClick( bool ) override
        {
            if ( PointCount() == 0 )
            {
                PushPoint( m_cur );
                UpdateHud();
                return true;
            }
            Finish( false );
            return false;
        }

        void Recompute() override
        {
            // A typed value sizes the rect SQUARE (one scalar, grammar v1): it is
            // the extent along both in-plane axes, signed by the drag direction.
            if ( m_hasNum && PointCount() == 1 )
            {
                float a[2], c[2];
                LastPointUV( a );
                CurUV( c );
                const float su = ( c[0] >= a[0] ) ? 1.0f : -1.0f;
                const float sv = ( c[1] >= a[1] ) ? 1.0f : -1.0f;
                c[0] = a[0] + su * m_numWorld;
                c[1] = a[1] + sv * m_numWorld;
                SetCurUV( c );
            }
            UpdateHud();
        }

        void UpdateHud() override
        {
            if ( PointCount() == 0 )
            {
                SetHud( "rect  ·  click: first corner" );
                return;
            }
            float a[2], c[2];
            LastPointUV( a );
            CurUV( c );
            char bu[32], bv[32];
            KiwiUnits_Format( bu, sizeof( bu ), fabsf( c[0] - a[0] ) );
            KiwiUnits_Format( bv, sizeof( bv ), fabsf( c[1] - a[1] ) );
            SetHud( "rect  %s x %s  ·  click: opposite corner  ·  type = square size", bu, bv );
        }

        void Finish( bool ) override
        {
            if ( PointCount() != 1 || !m_haveCur )
                return;
            float a[2], c[2];
            LastPointUV( a );
            CurUV( c );
            if ( fabsf( c[0] - a[0] ) < 1.0e-3f || fabsf( c[1] - a[1] ) < 1.0e-3f )
            {
                Sys_Printf( "Rect: zero extent — nothing placed.\n" );
                m_pts.clear();
                return;
            }

            kconObject_t o;
            o.type   = KCON_RECT;
            o.plane  = m_plane;
            o.closed = true;
            // CCW in plane space so a region derived from it needs no rewind; then
            // straight out to WORLD, which is what the store holds now.
            const float u0 = ( a[0] < c[0] ) ? a[0] : c[0];
            const float u1 = ( a[0] < c[0] ) ? c[0] : a[0];
            const float v0 = ( a[1] < c[1] ) ? a[1] : c[1];
            const float v1 = ( a[1] < c[1] ) ? c[1] : a[1];
            const float uv[4][2] = { { u0, v0 }, { u1, v0 }, { u1, v1 }, { u0, v1 } };
            for ( int i = 0; i < 4; ++i )
            {
                float w[3];
                KiwiCon_PlaneToWorld( m_plane, uv[i], w );
                o.pts.push_back( w[0] );  o.pts.push_back( w[1] );  o.pts.push_back( w[2] );
            }
            KiwiCon_UndoPush();
            KiwiCon_Add( o );
            m_pts.clear();
        }

        void DrawWorld() override
        {
            if ( PointCount() != 1 || !m_haveCur )
            {
                KiwiDrawTool::DrawWorld();
                return;
            }
            float a[2], cu[2];
            LastPointUV( a );
            CurUV( cu );
            const float c[4][2] = { { a[0], a[1] }, { cu[0], a[1] },
                                    { cu[0], cu[1] }, { a[0], cu[1] } };
            KiwiLines_Color( KCON_COL_ACTIVE[0], KCON_COL_ACTIVE[1], KCON_COL_ACTIVE[2] );
            for ( int i = 0; i < 4; ++i )
            {
                float w0[3], w1[3];
                KiwiCon_PlaneToWorld( m_plane, c[i], w0 );
                KiwiCon_PlaneToWorld( m_plane, c[( i + 1 ) & 3], w1 );
                if ( !KiwiLines_Add( w0, w1 ) )
                    return;
            }
        }
    };

    // ── CIRCLE — centre, then radius (click or typed) ───────────────────────
    class KiwiCircleTool : public KiwiDrawTool
    {
    public:
        const char *Name() const override { return "Construct Circle"; }

        // ROUND AF, ITEM 7 — "more sides on the circles".
        bool WantsSidesField() const override { return true; }

        // ═══════════════════════════════════════════════════════════════════
        //  ROUND AG, ITEM 5(a) — WHY ROUND AF'S FIELD WAS UNREACHABLE
        // ═══════════════════════════════════════════════════════════════════
        // USER REPORT, verbatim: "when making a circle, i still cant specify the
        // number of segments (only for cylinders?)."
        //
        // The field WAS attached (WantsSidesField above, KCON_FIELDS_LEN_SIDES),
        // Tab DID reach it, and the typed value DID flow.  What was missing is
        // that the field had nothing to SHOW, and in this HUD that is the same as
        // not existing: kiwi_numeric.cpp's FieldDisplay (:104-127) returns false
        // when the command's NumericFieldValue says no, and the bubble then
        // `continue`s past the row (:460-461).  The base's NumericFieldValue
        // refuses the sides field when ToolSides() is <= 0 (:1785), and the base's
        // ToolSides() returns m_sidesOverride — which is 0 until something has
        // been typed.  So: no row until you type, and no way to see that typing
        // would work.  Tab moved a caret onto an invisible box.
        //
        // "only for cylinders?" is the diagnosis to the letter — kiwi_primitive's
        // m_sides is seeded to KPRIM_CYL_SIDES_DEF and is therefore never 0, its
        // row always draws, and that tool has always been usable.
        //
        // The fix is to report the count the shape WILL HAVE, never 0: AUTO is an
        // answer, not an absence.  It also means the number in the box is the
        // number on the geometry at every instant, including before the first
        // click (radius 0 -> the KCON_SEGS_MIN floor), which is what makes "Tab,
        // type, click" work as a sentence.
        int ToolSides() const override
        {
            return ( m_sidesOverride > 0 ) ? CardinalSegs( m_sidesOverride )
                                           : CircleSegs( m_radius );
        }

        bool OnClick( bool ) override
        {
            if ( PointCount() == 0 )
            {
                PushPoint( m_cur );
                UpdateHud();
                return true;
            }
            Finish( false );
            return false;
        }

        void Recompute() override
        {
            // ROUND AF, ITEM 7 (compile fix): the circle has no m_sides of its own —
            // a typed count lands in the BASE's m_sidesOverride (NumericFieldChanged,
            // :1853) and every consumer reads it through CircleSegsFor(o.segs).  The
            // n-gon-pattern assignment that stood here referenced members only the
            // n-gon tool has.
            m_radius = 0.0f;
            if ( PointCount() == 1 )
            {
                float c[2], cur[2];
                LastPointUV( c );
                CurUV( cur );
                const float d0 = cur[0] - c[0], d1 = cur[1] - c[1];
                m_radius = m_hasNum ? m_numWorld : sqrtf( d0 * d0 + d1 * d1 );
            }
            UpdateHud();
        }

        void UpdateHud() override
        {
            if ( PointCount() == 0 )
            {
                SetHud( "circle  ·  click: centre" );
                return;
            }
            char b[32];
            KiwiUnits_Format( b, sizeof( b ), m_radius );
            // ROUND AF, ITEM 7: the readout honors a typed sides override, exactly as
            // the geometry will (CircleSegsFor) — a HUD that said "12 segs" while the
            // override was 48 would be lying about the shape under the cursor.
            // ROUND AG, ITEM 5: ToolSides() is the one place that answers "how many
            // segments will this circle have", so the HUD, the Tab field and the
            // geometry cannot disagree.  It is also where the multiple-of-four
            // rounding becomes visible, which is why the strip SAYS so — typing 30
            // and getting 32 has to be an announcement, not a surprise.
            const int hudSegs = ToolSides();
            SetHud( "circle  radius %s  (%i segs, x4 for the quadrants)  ·  "
                    "click: rim  ·  type = radius  ·  Tab = sides",
                    b, hudSegs );
        }

        void Finish( bool ) override
        {
            if ( PointCount() != 1 )
                return;
            if ( !( m_radius > 1.0e-3f ) )
            {
                Sys_Printf( "Circle: zero radius — nothing placed.\n" );
                m_pts.clear();
                return;
            }
            float c[2];
            LastPointUV( c );
            kconObject_t o;
            o.type      = KCON_CIRCLE;
            o.plane     = m_plane;
            o.closed    = true;
            o.centre[0] = c[0];
            o.centre[1] = c[1];
            o.radius    = m_radius;
            o.ang0      = 0.0f;
            o.ang1      = 360.0f;
            o.segs      = m_sidesOverride;    // ROUND AF, ITEM 7 (0 = automatic)
            KiwiCon_UndoPush();
            KiwiCon_Add( o );
            m_pts.clear();
        }

        void DrawWorld() override
        {
            if ( PointCount() != 1 || !( m_radius > 1.0e-3f ) )
            {
                KiwiDrawTool::DrawWorld();
                return;
            }
            float c[2];
            LastPointUV( c );
            kconObject_t preview;
            preview.type      = KCON_CIRCLE;
            preview.plane     = m_plane;
            preview.closed    = true;
            preview.centre[0] = c[0];
            preview.centre[1] = c[1];
            preview.radius    = m_radius;
            // ── KIWI-UX (ROUND AQ, ITEM 6): THE PREVIEW WAS THE ODD ONE OUT ──
            // USER REPORT, verbatim: "when drawing a circle, the number of sides
            // is inaccurate until it's tab-navigated to."  Round AG fixed the
            // FIELD (it used to show nothing until you typed) and round AF seeded
            // m_sidesOverride from the remembered RoundToolSides at Begin() — but
            // this preview builds a THROWAWAY kconObject_t and never assigned its
            // `segs`, so it kept the struct default 0 and KiwiCon_SegmentCount
            // resolved 0 as AUTO (CircleSegs(radius), a radius-driven count).  The
            // field, the HUD and the object placed on click were all already
            // saying m_sidesOverride; only the rubber band disagreed, and it
            // snapped to the stated count at commit — "inaccurate until tabbed to",
            // exactly.  Assigning it here makes displayed == tessellated at every
            // moment of the gesture, and 0 still means AUTO for both.
            preview.segs      = m_sidesOverride;
            KiwiLines_Color( KCON_COL_ACTIVE[0], KCON_COL_ACTIVE[1], KCON_COL_ACTIVE[2] );
            const int segs = KiwiCon_SegmentCount( preview );
            for ( int i = 0; i < segs; ++i )
            {
                float a[3], b[3];
                if ( !KiwiCon_SegmentWorld( preview, i, a, b ) )
                    break;
                if ( !KiwiLines_Add( a, b ) )
                    return;
            }
        }

    private:
        float m_radius = 0.0f;
    };

    // ── ARC — centre, then radius+start angle, then end angle ───────────────
    class KiwiArcTool : public KiwiDrawTool
    {
    public:
        const char *Name() const override { return "Construct Arc"; }

        // ROUND AF, ITEM 7 — the same tessellation field the circle carries.
        bool WantsSidesField() const override { return true; }

        // ROUND AG, ITEM 5(a): AUTO is an answer, not an absence — see the long
        // note on KiwiCircleTool::ToolSides.  This is the FULL-CIRCLE density the
        // arc is cut pro rata from (ArcSegs), which is the number the field sets
        // and therefore the number it must show.
        int ToolSides() const override
        {
            // KIWI-UX (ROUND AQ, ITEM 6): CardinalSegs, like the circle's.  The
            // arc's GEOMETRY runs ArcSegs -> CircleSegsFor, and CircleSegsFor
            // rounds a typed count up to a multiple of 4 (:258) — so without this
            // the field said "30" while the tessellation used 32.  Same class of
            // bug as the preview one below: displayed must equal tessellated.
            return ( m_sidesOverride > 0 ) ? CardinalSegs( m_sidesOverride )
                                           : CircleSegs( m_radius );
        }

        bool Begin() override
        {
            m_stage  = 0;
            m_radius = 0.0f;
            m_a0     = 0.0f;
            m_a1     = 0.0f;
            return KiwiDrawTool::Begin();
        }

        bool OnClick( bool ) override
        {
            if ( m_stage == 0 )                    // centre
            {
                PushPoint( m_cur );
                m_stage = 1;
                UpdateHud();
                return true;
            }
            if ( m_stage == 1 )                    // radius + start angle
            {
                if ( !( m_radius > 1.0e-3f ) )
                {
                    Sys_Printf( "Arc: zero radius — pick a point away from the centre.\n" );
                    return true;
                }
                m_lockRadius = m_radius;
                m_lockA0     = m_a0;
                m_stage      = 2;
                // SHAKEOUT C: clear the typed buffer on the stage change.  Stage 1's
                // number is a RADIUS and stage 2's is a SWEEP IN DEGREES; carrying
                // the radius across silently reinterpreted it as an angle the moment
                // the second click landed.  The framework resets per COMMAND
                // (KiwiCmd_Start), which is the right granularity for a one-scalar
                // gesture and the wrong one for a staged tool — so a staged tool
                // clears it itself.  kiwi_primitive.cpp does the same.
                //
                // SHAKEOUT E: KiwiNum_ClearEntry, not KiwiNum_Reset — Reset also
                // reinstalls the DEFAULT field table and would throw this tool's
                // own fields away mid-gesture (kiwi_numeric.h).
                KiwiNum_ClearEntry();
                m_hasNum   = false;
                m_numWorld = 0.0f;
                UpdateHud();
                return true;
            }
            Finish( false );                       // third click closes the sweep
            return false;
        }

        // KIWI-UX (ROUND AQ, ITEM 8): the arc is the construction-side twin of the
        // primitives' three-stage machine, so it takes the same Enter rule — a
        // typed radius at stage 1 advances to the sweep stage instead of finishing
        // the arc at zero sweep.  Recompute has already folded the number into
        // m_radius by the time this runs, which is why the latch below captures it
        // and the clear cannot lose it.  Stage 2 (the sweep) is the last one, so
        // Enter there still means confirm and this returns false.  See
        // kiwi_command.h AdvanceStage.
        bool AdvanceStage() override
        {
            if ( m_stage == 0 )
            {
                PushPoint( m_cur );
                m_stage = 1;
                UpdateHud();
                return true;
            }
            if ( m_stage == 1 )
            {
                if ( !( m_radius > 1.0e-3f ) )
                {
                    Sys_Printf( "Arc: zero radius — type one, or pick a point away "
                                "from the centre.\n" );
                    return true;                   // consumed; do NOT fall through to Finish
                }
                m_lockRadius = m_radius;
                m_lockA0     = m_a0;
                m_stage      = 2;
                KiwiNum_ClearEntry();
                m_hasNum   = false;
                m_numWorld = 0.0f;
                UpdateHud();
                return true;
            }
            return false;                          // stage 2 is final — Enter confirms
        }

        void Recompute() override
        {
            if ( m_stage >= 1 && PointCount() == 1 )
            {
                float c[2], cur[2];
                LastPointUV( c );
                CurUV( cur );
                const float d0 = cur[0] - c[0], d1 = cur[1] - c[1];
                const float ang = atan2f( d1, d0 ) * 57.29577951f;
                if ( m_stage == 1 )
                {
                    m_radius = m_hasNum ? m_numWorld : sqrtf( d0 * d0 + d1 * d1 );
                    m_a0     = ang;
                    m_a1     = ang;
                }
                else
                {
                    m_radius = m_lockRadius;
                    m_a0     = m_lockA0;
                    // Sweep CCW from the start; a typed value is the sweep in degrees
                    // (an arc's second scalar is an angle, not a length — the numeric
                    // layer hands out world units, so undo its inches conversion).
                    float sweep = m_hasNum ? Units_ToDisplay( m_numWorld ) : ( ang - m_a0 );
                    while ( sweep <    0.0f ) sweep += 360.0f;
                    while ( sweep >  360.0f ) sweep -= 360.0f;
                    m_a1 = m_a0 + sweep;
                }
            }
            UpdateHud();
        }

        void UpdateHud() override
        {
            if ( m_stage == 0 )
            {
                SetHud( "arc  ·  click: centre" );
                return;
            }
            char b[32];
            KiwiUnits_Format( b, sizeof( b ), m_radius );
            if ( m_stage == 1 )
                SetHud( "arc  radius %s  start %.1f deg  ·  click: start point  ·  type = radius",
                        b, (double)m_a0 );
            else
                SetHud( "arc  radius %s  sweep %.1f deg  ·  click: end point  ·  type = sweep (deg)",
                        b, (double)( m_a1 - m_a0 ) );
        }

        void Finish( bool ) override
        {
            if ( m_stage < 2 || PointCount() != 1 )
            {
                m_pts.clear();
                return;
            }
            if ( fabsf( m_a1 - m_a0 ) < 0.5f || !( m_radius > 1.0e-3f ) )
            {
                Sys_Printf( "Arc: zero sweep — nothing placed.\n" );
                m_pts.clear();
                return;
            }
            float c[2];
            LastPointUV( c );
            kconObject_t o;
            o.type      = KCON_ARC;
            o.plane     = m_plane;
            o.closed    = false;
            o.centre[0] = c[0];
            o.centre[1] = c[1];
            o.radius    = m_radius;
            o.ang0      = m_a0;
            o.ang1      = m_a1;
            o.segs      = m_sidesOverride;    // ROUND AF, ITEM 7 (0 = automatic)
            KiwiCon_UndoPush();
            KiwiCon_Add( o );
            m_pts.clear();
            m_stage = 0;
        }

        void DrawWorld() override
        {
            if ( m_stage < 1 || PointCount() != 1 || !( m_radius > 1.0e-3f ) )
            {
                KiwiDrawTool::DrawWorld();
                return;
            }
            float c[2];
            LastPointUV( c );
            kconObject_t preview;
            preview.type      = ( m_stage == 1 ) ? KCON_LINE : KCON_ARC;
            preview.plane     = m_plane;
            preview.centre[0] = c[0];
            preview.centre[1] = c[1];
            preview.radius    = m_radius;
            preview.ang0      = m_a0;
            preview.ang1      = ( m_stage == 1 ) ? m_a0 : m_a1;
            preview.segs      = m_sidesOverride;   // KIWI-UX (ROUND AQ, ITEM 6) — see KiwiCircleTool

            KiwiLines_Color( KCON_COL_ACTIVE[0], KCON_COL_ACTIVE[1], KCON_COL_ACTIVE[2] );
            if ( m_stage == 1 )
            {
                // Stage 1 shows the radius handle, not a zero-length arc.
                float w0[3];
                KiwiCon_PlaneToWorld( m_plane, c, w0 );
                KiwiLines_Add( w0, m_cur );
                return;
            }
            const int segs = KiwiCon_SegmentCount( preview );
            for ( int i = 0; i < segs; ++i )
            {
                float a[3], b[3];
                if ( !KiwiCon_SegmentWorld( preview, i, a, b ) )
                    break;
                if ( !KiwiLines_Add( a, b ) )
                    return;
            }
        }

    private:
        int   m_stage      = 0;
        float m_radius     = 0.0f;
        float m_a0         = 0.0f;
        float m_a1         = 0.0f;
        float m_lockRadius = 0.0f;
        float m_lockA0     = 0.0f;
    };

    // ═══════════════════════════════════════════════════════════════════════
    //  §16b (shakeout C) — the rest of the Plasticity curve inventory.
    //  RADIANT_UX_DESIGN §16b.3 is the map; each class names the Plasticity
    //  command it transliterates.
    // ═══════════════════════════════════════════════════════════════════════

    // ── RECTANGLE (CENTER) — Plasticity CenterRectangleCommand ──────────────
    //    (plasticity/src/commands/rect/RectangleCommand.ts:97: centre first,
    //     then a corner; the rect is mirrored through the centre.)
    class KiwiRectCenterTool : public KiwiDrawTool
    {
    public:
        const char *Name() const override { return "Construct Rectangle (center)"; }

        bool OnClick( bool ) override
        {
            if ( PointCount() == 0 )
            {
                PushPoint( m_cur );
                UpdateHud();
                return true;
            }
            Finish( false );
            return false;
        }

        void Recompute() override
        {
            // NUMERIC RULE, and it is deliberately NOT the corner tool's:
            // a typed value here is the HALF-SIZE — centre to edge — so the rect
            // comes out 2N x 2N.  Reason: in centre mode the number the user is
            // thinking about is the one the cursor is showing them, and the
            // cursor measures from the centre.  The HUD says "type = half-size"
            // so the two rect tools can never be confused for each other.
            if ( m_hasNum && PointCount() == 1 )
            {
                float c[2], cur[2];
                LastPointUV( c );
                CurUV( cur );
                const float su = ( cur[0] >= c[0] ) ? 1.0f : -1.0f;
                const float sv = ( cur[1] >= c[1] ) ? 1.0f : -1.0f;
                cur[0] = c[0] + su * m_numWorld;
                cur[1] = c[1] + sv * m_numWorld;
                SetCurUV( cur );
            }
            UpdateHud();
        }

        void UpdateHud() override
        {
            if ( PointCount() == 0 )
            {
                SetHud( "rect (center)  ·  click: centre" );
                return;
            }
            float c[2], cur[2];
            LastPointUV( c );
            CurUV( cur );
            char bu[32], bv[32];
            KiwiUnits_Format( bu, sizeof( bu ), fabsf( cur[0] - c[0] ) * 2.0f );
            KiwiUnits_Format( bv, sizeof( bv ), fabsf( cur[1] - c[1] ) * 2.0f );
            SetHud( "rect (center)  %s x %s  ·  click: corner  ·  type = half-size", bu, bv );
        }

        void Finish( bool ) override
        {
            if ( PointCount() != 1 || !m_haveCur )
                return;
            float c[2], cur[2];
            LastPointUV( c );
            CurUV( cur );
            const float hu = fabsf( cur[0] - c[0] );
            const float hv = fabsf( cur[1] - c[1] );
            if ( hu < 1.0e-3f || hv < 1.0e-3f )
            {
                Sys_Printf( "Rect (center): zero extent — nothing placed.\n" );
                m_pts.clear();
                return;
            }

            kconObject_t o;
            o.type   = KCON_RECT;
            o.plane  = m_plane;
            o.closed = true;
            // CCW, same convention as KiwiRectTool; out to WORLD as the store wants.
            const float uv[4][2] = { { c[0] - hu, c[1] - hv }, { c[0] + hu, c[1] - hv },
                                     { c[0] + hu, c[1] + hv }, { c[0] - hu, c[1] + hv } };
            for ( int i = 0; i < 4; ++i )
            {
                float w[3];
                KiwiCon_PlaneToWorld( m_plane, uv[i], w );
                o.pts.push_back( w[0] );  o.pts.push_back( w[1] );  o.pts.push_back( w[2] );
            }
            KiwiCon_UndoPush();
            KiwiCon_Add( o );
            m_pts.clear();
        }

        void DrawWorld() override
        {
            if ( PointCount() != 1 || !m_haveCur )
            {
                KiwiDrawTool::DrawWorld();
                return;
            }
            float c[2], cur[2];
            LastPointUV( c );
            CurUV( cur );
            const float hu = fabsf( cur[0] - c[0] );
            const float hv = fabsf( cur[1] - c[1] );
            const float q[4][2] = { { c[0] - hu, c[1] - hv }, { c[0] + hu, c[1] - hv },
                                    { c[0] + hu, c[1] + hv }, { c[0] - hu, c[1] + hv } };
            KiwiLines_Color( KCON_COL_ACTIVE[0], KCON_COL_ACTIVE[1], KCON_COL_ACTIVE[2] );
            for ( int i = 0; i < 4; ++i )
            {
                float w0[3], w1[3];
                KiwiCon_PlaneToWorld( m_plane, q[i], w0 );
                KiwiCon_PlaneToWorld( m_plane, q[( i + 1 ) & 3], w1 );
                if ( !KiwiLines_Add( w0, w1 ) )
                    return;
            }
        }
    };

    // ── CIRCLE (2-POINT) — Plasticity TwoPointCircleCommand ─────────────────
    //    (plasticity/src/commands/circle/CircleCommand.ts:79: the two clicks are
    //     the ENDS OF A DIAMETER, so the centre is their midpoint.)
    class KiwiCircle2PtTool : public KiwiDrawTool
    {
    public:
        const char *Name() const override { return "Construct Circle (2-point)"; }

        // ROUND AF, ITEM 7 — the same tessellation field the circle carries.
        bool WantsSidesField() const override { return true; }

        // ROUND AG, ITEM 5 — see KiwiCircleTool::ToolSides.  Same shape, same
        // cardinal rounding: this tool emits a KCON_CIRCLE too.
        int ToolSides() const override
        {
            return ( m_sidesOverride > 0 ) ? CardinalSegs( m_sidesOverride )
                                           : CircleSegs( m_radius );
        }

        bool OnClick( bool ) override
        {
            if ( PointCount() == 0 )
            {
                PushPoint( m_cur );
                UpdateHud();
                return true;
            }
            Finish( false );
            return false;
        }

        void Recompute() override
        {
            m_radius = 0.0f;
            if ( PointCount() == 1 )
            {
                float a[2], cur[2];
                LastPointUV( a );
                CurUV( cur );
                float d[2] = { cur[0] - a[0], cur[1] - a[1] };
                const float len = sqrtf( d[0] * d[0] + d[1] * d[1] );
                // NUMERIC RULE: a typed value is the DIAMETER (the distance
                // between the two clicks), because that is literally what the
                // gesture measures.  The second point is pushed out along the
                // current direction to honour it.
                if ( m_hasNum && len > 1.0e-4f )
                {
                    cur[0] = a[0] + d[0] / len * m_numWorld;
                    cur[1] = a[1] + d[1] / len * m_numWorld;
                    SetCurUV( cur );
                    d[0] = cur[0] - a[0];
                    d[1] = cur[1] - a[1];
                }
                m_radius = 0.5f * sqrtf( d[0] * d[0] + d[1] * d[1] );
                m_centre[0] = ( a[0] + cur[0] ) * 0.5f;
                m_centre[1] = ( a[1] + cur[1] ) * 0.5f;
            }
            UpdateHud();
        }

        void UpdateHud() override
        {
            if ( PointCount() == 0 )
            {
                SetHud( "circle (2-point)  ·  click: first diameter end" );
                return;
            }
            char br[32], bd[32];
            KiwiUnits_Format( br, sizeof( br ), m_radius );
            KiwiUnits_Format( bd, sizeof( bd ), m_radius * 2.0f );
            // ROUND AG, ITEM 5: through ToolSides, so a typed override shows here
            // too (it did not before) and the x4 rounding is announced.
            SetHud( "circle (2-point)  dia %s  r %s  (%i segs, x4 for the quadrants)  ·  "
                    "click: second end  ·  type = diameter  ·  Tab = sides",
                    bd, br, ToolSides() );
        }

        void Finish( bool ) override
        {
            if ( PointCount() != 1 )
                return;
            if ( !( m_radius > 1.0e-3f ) )
            {
                Sys_Printf( "Circle (2-point): zero diameter — nothing placed.\n" );
                m_pts.clear();
                return;
            }
            kconObject_t o;
            o.type      = KCON_CIRCLE;
            o.plane     = m_plane;
            o.closed    = true;
            o.centre[0] = m_centre[0];
            o.centre[1] = m_centre[1];
            o.radius    = m_radius;
            o.ang0      = 0.0f;
            o.ang1      = 360.0f;
            o.segs      = m_sidesOverride;    // ROUND AF, ITEM 7 (0 = automatic)
            KiwiCon_UndoPush();
            KiwiCon_Add( o );
            m_pts.clear();
        }

        void DrawWorld() override
        {
            if ( PointCount() != 1 || !( m_radius > 1.0e-3f ) )
            {
                KiwiDrawTool::DrawWorld();
                return;
            }
            kconObject_t preview;
            preview.type      = KCON_CIRCLE;
            preview.plane     = m_plane;
            preview.closed    = true;
            preview.centre[0] = m_centre[0];
            preview.centre[1] = m_centre[1];
            preview.radius    = m_radius;
            preview.segs      = m_sidesOverride;   // KIWI-UX (ROUND AQ, ITEM 6) — see KiwiCircleTool
            KiwiLines_Color( KCON_COL_ACTIVE[0], KCON_COL_ACTIVE[1], KCON_COL_ACTIVE[2] );
            const int segs = KiwiCon_SegmentCount( preview );
            for ( int i = 0; i < segs; ++i )
            {
                float a[3], b[3];
                if ( !KiwiCon_SegmentWorld( preview, i, a, b ) )
                    break;
                if ( !KiwiLines_Add( a, b ) )
                    return;
            }
            // The diameter handle, so the gesture reads as "two ends", not "rim".
            float first[3];
            FirstPoint( first );
            KiwiLines_Add( first, m_cur );
        }

    private:
        float m_radius    = 0.0f;
        float m_centre[2] = { 0.0f, 0.0f };
    };

    // ── POLYGON (n-gon) — Plasticity PolygonCommand ─────────────────────────
    //    (plasticity/src/commands/polygon/PolygonCommand.ts: centre then radius,
    //     with a keyboard gizmo adding/removing vertices mid-command — bound
    //     there to shift-wheel±.)
    //
    // ── HOW THE DIGITS AND THE SIDE COUNT COEXIST ───────────────────────────
    // They do not compete, by construction.  The framework feeds EVERY digit to
    // the numeric entry before a command's KeyDown ever sees it
    // (kiwi_command.cpp KiwiCmd_KeyDown rung 1), so a typed number can only ever
    // mean the RADIUS — the §13 grammar's one scalar.  The side count therefore
    // takes a NON-DIGIT input: `[` decrements and `]` increments, clamped to
    // KCON_POLY_SIDES_MIN..MAX, default KCON_POLY_SIDES_DEF.
    //   * chosen over Plasticity's shift-wheel because the wheel is the camera
    //     dolly here and §4 forbids a command taking it (kiwi_command.h: the
    //     wheel is never offered to a command at all);
    //   * chosen over an "N toggles which field the digits feed" mode because a
    //     hidden mode with no visible state is exactly the thing the HUD rule
    //     exists to prevent — you would have to type to find out where it went.
    // While the tool runs, `[` / `]` are SHADOWED from their modern grid-spacing
    // bindings: a modal command owns its keys (§4), and the framework swallows
    // whatever the command consumes.  The HUD says so on every frame.
    class KiwiPolygonTool : public KiwiDrawTool
    {
    public:
        const char *Name() const override { return "Construct Polygon"; }

        bool Begin() override
        {
            // ROUND AF, ITEM 7: the [ ] count and the typed field are ONE number
            // now, held in the base's m_sidesOverride (which Begin has already
            // seeded from the remembered per-session value).  m_sides is what the
            // rest of this class reads and is derived from it, so the two controls
            // can never disagree.
            m_radius = 0.0f;
            m_ang0   = 0.0f;
            // The base seeds m_sidesOverride from the remembered value, so it has to
            // run FIRST — PolySides reads what it wrote.
            const bool ok = KiwiDrawTool::Begin();
            m_sides = PolySides();
            return ok;
        }

        bool KeyDown( int vk, unsigned int mods ) override
        {
            if ( !mods && ( vk == 0xDB || vk == 0xDD ) )   // VK_OEM_4 '[' / VK_OEM_6 ']'
            {
                m_sides += ( vk == 0xDD ) ? 1 : -1;
                if ( m_sides < KCON_POLY_SIDES_MIN ) m_sides = KCON_POLY_SIDES_MIN;
                if ( m_sides > KCON_POLY_SIDES_MAX ) m_sides = KCON_POLY_SIDES_MAX;
                // ROUND AF, ITEM 7: keep the Tab field and the session memory in
                // step — the keys and the box are two views of one number.
                m_sidesOverride = m_sides;
                KiwiCon_SetToolSides( m_sides );
                UpdateHud();
                g_nUpdateBits |= 1;
                return true;
            }
            return KiwiDrawTool::KeyDown( vk, mods );
        }

        bool OnClick( bool ) override
        {
            if ( PointCount() == 0 )
            {
                PushPoint( m_cur );
                UpdateHud();
                return true;
            }
            Finish( false );
            return false;
        }

        void Recompute() override
        {
            // ── KIWI-UX (ROUND AQ, ITEM 6): THE COMMENT WAS TRUE, THE CODE WAS NOT ──
            // The note below m_sides claimed "Recompute folds the base's number back
            // in".  It did not — m_sides was only re-derived in Begin(), so a count
            // TYPED into the Tab field (which lands in the base's m_sidesOverride via
            // KiwiDrawTool::NumericFieldChanged) changed nothing until the tool was
            // restarted: the readout, the preview and the committed vertex list all
            // read m_sides and all kept the old number.  The `[`/`]` keys worked only
            // because they write m_sides directly.  This is the same report as the
            // circle's — a side count that is not what the tool actually builds — so
            // it is fixed with it.  PolySides() already carries the clamp.
            m_sides  = PolySides();
            m_radius = 0.0f;
            if ( PointCount() == 1 )
            {
                float c[2], cur[2];
                LastPointUV( c );
                CurUV( cur );
                const float d0 = cur[0] - c[0], d1 = cur[1] - c[1];
                // The cursor direction is the FIRST VERTEX's direction, so the
                // n-gon rotates with the drag (Plasticity's polygon does the same
                // through its `orientation`).  A typed value replaces the radius
                // only — never the angle.
                m_ang0   = atan2f( d1, d0 );
                m_radius = m_hasNum ? m_numWorld : sqrtf( d0 * d0 + d1 * d1 );
            }
            UpdateHud();
        }

        void UpdateHud() override
        {
            if ( PointCount() == 0 )
            {
                SetHud( "polygon  %i sides  ·  click: centre  ·  [ ] change sides", m_sides );
                return;
            }
            char b[32];
            KiwiUnits_Format( b, sizeof( b ), m_radius );
            SetHud( "polygon  %i sides  r %s  ·  click: radius  ·  type = radius  ·  [ ] sides",
                    m_sides, b );
        }

        void Finish( bool ) override
        {
            if ( PointCount() != 1 )
                return;
            if ( !( m_radius > 1.0e-3f ) )
            {
                Sys_Printf( "Polygon: zero radius — nothing placed.\n" );
                m_pts.clear();
                return;
            }
            float c[2];
            LastPointUV( c );

            // Stored as a CLOSED POLYLINE (kiwi_construct.h §16b note): no new
            // store type, no sidecar keyword, and the region/extrude path already
            // understands it.  Wound CCW in plane space, matching KiwiRectTool,
            // then emitted in WORLD (shakeout H ruling 3).
            kconObject_t o;
            o.type   = KCON_POLYLINE;
            o.plane  = m_plane;
            o.closed = true;
            o.pts.reserve( (size_t)m_sides * 3 );
            for ( int i = 0; i < m_sides; ++i )
            {
                const float a = m_ang0 + 6.283185307f * (float)i / (float)m_sides;
                const float uv[2] = { c[0] + cosf( a ) * m_radius,
                                      c[1] + sinf( a ) * m_radius };
                float w[3];
                KiwiCon_PlaneToWorld( m_plane, uv, w );
                o.pts.push_back( w[0] );  o.pts.push_back( w[1] );  o.pts.push_back( w[2] );
            }
            KiwiCon_UndoPush();
            KiwiCon_Add( o );
            m_pts.clear();
        }

        void DrawWorld() override
        {
            if ( PointCount() != 1 || !( m_radius > 1.0e-3f ) )
            {
                KiwiDrawTool::DrawWorld();
                return;
            }
            float c[2];
            LastPointUV( c );
            KiwiLines_Color( KCON_COL_ACTIVE[0], KCON_COL_ACTIVE[1], KCON_COL_ACTIVE[2] );
            for ( int i = 0; i < m_sides; ++i )
            {
                const float a0 = m_ang0 + 6.283185307f * (float)i / (float)m_sides;
                const float a1 = m_ang0 + 6.283185307f * (float)( i + 1 ) / (float)m_sides;
                const float uv0[2] = { c[0] + cosf( a0 ) * m_radius, c[1] + sinf( a0 ) * m_radius };
                const float uv1[2] = { c[0] + cosf( a1 ) * m_radius, c[1] + sinf( a1 ) * m_radius };
                float w0[3], w1[3];
                KiwiCon_PlaneToWorld( m_plane, uv0, w0 );
                KiwiCon_PlaneToWorld( m_plane, uv1, w1 );
                if ( !KiwiLines_Add( w0, w1 ) )
                    return;
            }
        }

    private:
        // ROUND AF, ITEM 7: the n-gon's own field/[ ] plumbing.  It overrides
        // ToolSides so the readout shows a real number even before the base's
        // override has been touched (an n-gon is never "automatic" — it always has
        // a definite side count), and Recompute folds the base's number back in.
        bool WantsSidesField() const override { return true; }
        int  ToolSides() const override       { return m_sides; }
        int  PolySides() const
        {
            int n = ( m_sidesOverride > 0 ) ? m_sidesOverride : KCON_POLY_SIDES_DEF;
            if ( n < KCON_POLY_SIDES_MIN ) n = KCON_POLY_SIDES_MIN;
            if ( n > KCON_POLY_SIDES_MAX ) n = KCON_POLY_SIDES_MAX;
            return n;
        }

        int   m_sides  = KCON_POLY_SIDES_DEF;
        float m_radius = 0.0f;
        float m_ang0   = 0.0f;
    };

    // ── SPLINE — Plasticity CurveCommand ────────────────────────────────────
    //    (plasticity/src/commands/curve/CurveCommand.ts:10 — control-point
    //     clicks, Enter finishes, clicking the first point closes.  Plasticity
    //     keeps a real NURBS/Hermite curve; the store keeps its TESSELLATION,
    //     for the reason in the tessellation note above.)
    class KiwiSplineTool : public KiwiDrawTool
    {
    public:
        const char *Name() const override { return "Construct Spline"; }
        bool WantsEnterFinish() const override { return PointCount() >= 2; }
        bool PlanarOnly() const override { return false; }   // shakeout H: free 3D

        bool Begin() override
        {
            m_wantClosed = false;
            return KiwiDrawTool::Begin();
        }

        bool OnClick( bool doubleClick ) override
        {
            if ( PointCount() >= 3 && NearFirstPoint() )
            {
                m_wantClosed = true;
                Finish( false );
                return false;
            }
            if ( doubleClick && PointCount() >= 2 )
            {
                Finish( false );
                return false;
            }
            // The CONTROL-POINT cap, not the store's point cap: the tessellation
            // is what has to fit KCON_MAX_POINTS, and TessellateSpline coarsens to
            // make it.  A spline with more control points than that would be a
            // polyline the user drew by hand.
            if ( PointCount() >= KCON_MAX_POINTS / KCON_SPLINE_SEGS )
            {
                Sys_Printf( "Spline: control-point limit reached — ending here.\n" );
                Finish( false );
                return false;
            }
            PushPoint( m_cur );
            UpdateHud();
            return true;
        }

        void Recompute() override
        {
            if ( m_hasNum && PointCount() >= 1 )
            {
                float a[3], d[3];
                LastPoint( a );
                Sub3( m_cur, a, d );
                const float l = Len3( d );
                if ( l > 1.0e-4f )
                    Mad3( a, d, m_numWorld / l, m_cur );
            }
            UpdateHud();
        }

        void UpdateHud() override
        {
            const int n = PointCount();
            if ( n == 0 )
            {
                SetHud( "spline  ·  click: first control point" );
                return;
            }
            SetHud( "spline  %i pts  (%i segs/span)%s  ·  %s  ·  Z: vertical %s",
                    n, KCON_SPLINE_SEGS, m_zLock ? "  [Z LOCK]" : "",
                    ( n >= 3 && NearFirstPoint() )
                        ? "click: CLOSE the curve"
                        : "click: next point  ·  RMB/Enter: finish",
                    m_zLock ? "ON" : "off" );
        }

        void Finish( bool ) override
        {
            std::vector<float> ctrl = m_pts;
            if ( !m_wantClosed && m_haveCur )
            {
                ctrl.push_back( m_cur[0] );
                ctrl.push_back( m_cur[1] );
                ctrl.push_back( m_cur[2] );
            }
            if ( (int)( ctrl.size() / 3 ) < 2 )
            {
                m_pts.clear();
                return;
            }

            kconObject_t o;
            o.type   = KCON_POLYLINE;
            o.plane  = m_plane;
            o.closed = m_wantClosed;
            TessellateSpline( ctrl, m_wantClosed, &o.pts );
            if ( (int)( o.pts.size() / 3 ) < 2 )
            {
                m_pts.clear();
                m_wantClosed = false;
                return;
            }
            KiwiCon_UndoPush();
            KiwiCon_Add( o );
            m_pts.clear();
            m_wantClosed = false;
        }

        void DrawWorld() override
        {
            std::vector<float> ctrl = m_pts;
            if ( m_haveCur )
            {
                ctrl.push_back( m_cur[0] );
                ctrl.push_back( m_cur[1] );
                ctrl.push_back( m_cur[2] );
            }
            if ( (int)( ctrl.size() / 3 ) < 2 )
            {
                KiwiDrawTool::DrawWorld();
                return;
            }
            std::vector<float> tess;
            TessellateSpline( ctrl, false, &tess );
            KiwiLines_Color( KCON_COL_ACTIVE[0], KCON_COL_ACTIVE[1], KCON_COL_ACTIVE[2] );
            const int n = (int)( tess.size() / 3 );
            for ( int i = 0; i + 1 < n; ++i )
                if ( !KiwiLines_Add( &tess[(size_t)i * 3], &tess[( (size_t)i + 1 ) * 3] ) )
                    return;
        }

    private:
        bool m_wantClosed = false;
    };

    // ROUND P: ONE object behind both the LINE and the POLYLINE command ids.
    KiwiCurveTool      s_curveTool;
    KiwiRectTool       s_rectTool;
    KiwiCircleTool     s_circleTool;
    KiwiArcTool        s_arcTool;
    KiwiRectCenterTool s_rectCenterTool;
    KiwiCircle2PtTool  s_circle2PtTool;
    KiwiPolygonTool    s_polygonTool;
    KiwiSplineTool     s_splineTool;
}

bool KiwiCon_ToolActive()
{
    return s_activeTool != 0;
}

// ── ROUND K: plane placement (see kiwi_construct.h for the box bug this fixes) ─
namespace
{
    bool s_planePlacement = false;
}

void KiwiCon_SetPlanePlacement( bool on )
{
    s_planePlacement = on;
}

bool KiwiCon_PlanePlacement()
{
    // A drawing tool IS a plane-placing command; asking one question keeps the two
    // from ever disagreeing about the snap arm they share.
    return s_activeTool != 0 || s_planePlacement;
}

bool KiwiCon_ToolAnchor( float out[3] )
{
    if ( !s_activeTool || s_activeTool->PointCount() < 1 )
        return false;
    s_activeTool->LastPoint( out );               // shakeout H: already world
    return true;
}

bool KiwiCon_ToolLoopStart( float out[3] )
{
    // ROUND AA, ITEM 3a — see kiwi_construct.h for the report and the reasoning.
    // Three points is the floor: two points cannot enclose anything, so there is
    // no loop to close and the "first point" is just the other end of the segment
    // being drawn — offering it as a magnet there would fight the drawing.
    if ( !s_activeTool || s_activeTool->PointCount() < 3 )
        return false;
    s_activeTool->FirstPoint( out );
    return true;
}

bool KiwiCon_ToolPrevAnchor( float out[3] )
{
    if ( !s_activeTool || s_activeTool->PointCount() < 2 )
        return false;
    const std::vector<float> &p = s_activeTool->m_pts;
    // Three floats per point, so the point before the last starts six back.
    Copy3( &p[p.size() - 6], out );
    return true;
}

// ─── draw (Cam_Draw tail) ────────────────────────────────────────────────────
namespace
{
    // World units per screen pixel at `world`.
    // ROUND M: this WAS a fourth private copy of the perspective per-pixel scale.
    // It is now a thin alias over KiwiCam_WorldPerPixel (as kiwi_hover.cpp's has
    // been since shakeout A), because that body carries the ORTHOGRAPHIC arm — in
    // an ortho view the scale is depth-INDEPENDENT, and a private copy would have
    // gone on shrinking these markers with distance in a projection that does not
    // shrink anything.  `c` is redundant (the shared body reads Ed_Camera()) but
    // keeps every call site below unchanged.
    inline float WorldPerPixel( const camera_s *c, const float *world )
    {
        (void)c;
        return KiwiCam_WorldPerPixel( world );
    }

    // ── KIWI-UX (ROUND U): THE ANCHOR IS A FILLED DOT, NOT AN X ─────────────
    // USER DIRECTIVE, verbatim: "instead of small x's on points of lines, could you
    // not do filled in circles like plasticity has?"
    //
    // A camera-facing regular polygon with its long diagonals filled in — the
    // EXACT trick kiwi_snap.cpp's marker and kiwi_lollipop.cpp's ball already use
    // to make a small polygon read as a SOLID dot with a LINE renderer and no fill
    // primitive.  At this size the diagonals overlap enough to look solid.
    //
    // SIZES, in pixels (screen-scaled through WorldPerPixel, so they hold at any
    // distance and in ortho):
    //     unselected   radius 2.5 px, 8 sides   (rose, KCON_COL_POINT)
    //     selected     radius 3.5 px, 8 sides   (white, KCON_COL_SEL_POINT)
    // "Slightly larger" and not a second glyph: the two passes already draw in
    // different colours, so size is the only extra signal needed and a selected
    // dot must still read as the same KIND of thing.
    //
    // COST: 8 outline segments + 4 diagonals = 12 per anchor, against the old
    // cross's 2 (+4 more for the selected square, which is deleted with it).  The
    // pass budget is KCON_DRAW_SEGMENTS (1600) and the per-anchor guard below was
    // raised from 6 to 16 to match.
    const int   KCON_DOT_SEGS      = 8;
    const float KCON_DOT_PIX       = 2.5f;
    const float KCON_DOT_PIX_SEL   = 3.5f;

    void DrawFilledDot( const camera_s *c, const float *p, float pixRadius )
    {
        const float r = WorldPerPixel( c, p ) * pixRadius;
        float prev[3], pt[3];
        for ( int i = 0; i <= KCON_DOT_SEGS; ++i )
        {
            const float th = ( 6.283185307179586f * (float)( i % KCON_DOT_SEGS ) )
                           / (float)KCON_DOT_SEGS;
            const float cx = cosf( th ) * r;
            const float cy = sinf( th ) * r;
            for ( int k = 0; k < 3; ++k )
                pt[k] = p[k] + c->vright[k] * cx + c->vup[k] * cy;
            if ( i > 0 )
                KiwiLines_Add( prev, pt );
            Copy3( pt, prev );
        }
        // The long diagonals: chord i to chord i+segs/2, which is what fills it.
        for ( int i = 0; i < KCON_DOT_SEGS / 2; ++i )
        {
            const float th0 = ( 6.283185307179586f * (float)i ) / (float)KCON_DOT_SEGS;
            const float th1 = th0 + 3.14159265358979f;
            float a[3], b[3];
            for ( int k = 0; k < 3; ++k )
            {
                a[k] = p[k] + c->vright[k] * cosf( th0 ) * r + c->vup[k] * sinf( th0 ) * r;
                b[k] = p[k] + c->vright[k] * cosf( th1 ) * r + c->vup[k] * sinf( th1 ) * r;
            }
            KiwiLines_Add( a, b );
        }
    }

    // ══════════════════════════════════════════════════════════════════════
    //  ROUND M — THE CONSTRUCTION-PLANE PATCH IS GONE.
    // ══════════════════════════════════════════════════════════════════════
    // USER DIRECTIVE, verbatim: "You still need to get rid of the weird grid that
    // shows up whenever you do a line command."
    //
    // §16 asked for a "subtle finite grid patch around the cursor" and this is the
    // third round spent trying to make it subtle: shakeout H took the pink out of
    // it and suppressed it on the ground plane, and it is still the thing the user
    // sees and dislikes.  The directive is unconditional — "whenever you do a line
    // command", not "when it is on the ground" — so DrawPlanePatch and its
    // WorkingPlaneIsGround helper are DELETED rather than gated again.
    //
    // WHAT REMAINS AS THE WORKING-PLANE FEEDBACK, i.e. why nothing is actually
    // lost: the SNAP MARKER (kiwi_snap.cpp, drawn at the resolved point with its
    // own glyph per snap kind) says where the cursor has landed, the NUMERIC
    // BUBBLE (kiwi_numeric.cpp) says how far and at what angle, the ACTIVE CHAIN
    // itself (drawn amber, below) says what plane the points are falling on, and
    // §17's ground grid is still there underneath.  The patch was the only one of
    // those five that had to be re-derived from a plane the user cannot see.
    //
    // SEPARABILITY, checked rather than assumed: the patch was gated on
    // `s_activeTool`, and the explicit "Construction Plane: XY/XZ/YZ/From Face/
    // From View" palette commands (kiwi_command.cpp:203-207) do not run a drawing
    // tool — they only re-seat the plane through KiwiCon_SetActivePlane.  So the
    // patch NEVER drew for them, and deleting it takes nothing away from that
    // half of §16 either.  KCON_COL_PLANE / KCON_COL_PLANEX / KCON_PATCH_CELLS go
    // with it.
}

void KiwiCon_DrawWorld()
{
    if ( !KiwiCon_ShowConstruction() )
        return;

    camera_s *c = Ed_Camera();
    if ( c->width < 1 || c->height < 1 )
        return;

    // ══════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND AQ, ITEM 2) — THE REGION FILLS NO LONGER DRAW FROM HERE.
    // ══════════════════════════════════════════════════════════════════════
    // `KiwiRegion_DrawFills( -1 )` used to be the first thing this pass did.  It
    // now runs from the COMMAND-OVERLAY SLOT in Cam_Draw's tail instead
    // (camwnd.cpp, immediately before KiwiCmd_DrawWorld) — the slot the BOOLEAN's
    // red operand preview draws from, which is the one fill in this editor the
    // user has confirmed visible.  Draw-pass LOCATION was the last structural
    // difference left between the two emitters after round AM equalised their
    // submission state; every other difference is now gone as well
    // (RADIANT_UX_DESIGN §69 carries the diff table).  If the fill appears, the
    // location was it.  If it does not, the submission-side model is dead and the
    // next round starts from the renderer.
    //
    // The gate is unchanged: the new call site re-checks KiwiCon_ShowConstruction()
    // exactly as this function does above, so the toggle still owns both.

    KiwiLines_Begin( KCON_DRAW_SEGMENTS, 1 );

    // (ROUND M: DrawPlanePatch() used to open this batch.  It is deleted — see the
    //  block above the KiwiCon_DrawWorld definition for the directive and for what
    //  carries the working-plane feedback now.)

    // KIWI-UX (shakeout F): SELECTED construction geometry reads brighter than the
    // rose base.  Near-white rather than a second hue on purpose — §18 already
    // spends cyan on hover, amber on the active gesture and magenta on selected
    // faces, and a fifth hue here would be one more thing to learn.  White also
    // matches what a SELECTED BRUSH already looks like, so "selected" means the
    // same thing to the eye on both kinds of geometry.
    //
    // Two passes rather than a colour change per object: KiwiLines_Color opens a
    // new colour RUN in the batch, so alternating per object would shred one draw
    // into as many runs as there are objects.  Unselected first, selected on top.
    for ( int pass = 0; pass < 2; ++pass )
    {
        const bool wantSel = ( pass == 1 );
        if ( wantSel ) KiwiLines_Color( KCON_COL_SEL_LINE[0],  KCON_COL_SEL_LINE[1],  KCON_COL_SEL_LINE[2] );
        else           KiwiLines_Color( KCON_COL_LINE[0],      KCON_COL_LINE[1],      KCON_COL_LINE[2] );

        for ( size_t i = 0; i < s_objects.size(); ++i )
        {
            const kconObject_t &o = s_objects[i];
            if ( o.hidden )                       // ROUND U — hidden is inert
                continue;
            const int segs = KiwiCon_SegmentCount( o );
            for ( int s = 0; s < segs; ++s )
            {
                if ( KiwiConSel_SegmentSelected( (int)i, s ) != wantSel )
                    continue;
                float a[3], b[3];
                if ( !KiwiCon_SegmentWorld( o, s, a, b ) )
                    break;
                if ( !KiwiLines_Add( a, b ) )
                {
                    KiwiLines_Flush();
                    return;
                }
            }
        }
    }

    // ── ROUND AG, ITEM 3: THE OUTLINER'S HOVERED CURVE ─────────────────────
    // USER DIRECTIVE: "While mousing over the brushes in the outliner, it should
    // highlight them in 3D so I can find them easier."  A curve is not a brush
    // and is not drawn by kiwi_hover.cpp, so its half of the highlight is HERE —
    // one more colour run, after both selection passes so it lands on top,
    // re-tracing the hovered object's (or folder's) whole polyline in the hover
    // cyan.  The target is published by the outliner and cleared every frame
    // (kiwi_hover.h THE CONTRACT), so nothing here has any state to keep.
    {
        const int hovObj = KiwiHover_OutlinerConIndex();
        const int hovGrp = KiwiHover_OutlinerConGroupId();
        if ( hovObj >= 0 || hovGrp != 0 )
        {
            KiwiLines_Color( KCON_COL_HOVER[0], KCON_COL_HOVER[1], KCON_COL_HOVER[2] );
            for ( size_t i = 0; i < s_objects.size(); ++i )
            {
                const kconObject_t &o = s_objects[i];
                if ( o.hidden )
                    continue;
                if ( (int)i != hovObj && !( hovGrp != 0 && o.group == hovGrp ) )
                    continue;
                const int segs = KiwiCon_SegmentCount( o );
                for ( int s = 0; s < segs; ++s )
                {
                    float a[3], b[3];
                    if ( !KiwiCon_SegmentWorld( o, s, a, b ) )
                        break;
                    if ( !KiwiLines_Add( a, b ) )
                    {
                        KiwiLines_Flush();
                        return;
                    }
                }
            }
        }
    }

    for ( int pass = 0; pass < 2; ++pass )
    {
        const bool wantSel = ( pass == 1 );
        if ( wantSel ) KiwiLines_Color( KCON_COL_SEL_POINT[0], KCON_COL_SEL_POINT[1], KCON_COL_SEL_POINT[2] );
        else           KiwiLines_Color( KCON_COL_POINT[0],     KCON_COL_POINT[1],     KCON_COL_POINT[2] );

        for ( size_t i = 0; i < s_objects.size(); ++i )
        {
            const kconObject_t &o = s_objects[i];
            if ( o.hidden )                       // ROUND U — hidden is inert
                continue;
            const int anchors = KiwiCon_AnchorCount( o );
            for ( int a = 0; a < anchors; ++a )
            {
                if ( KiwiConSel_PointSelected( (int)i, a ) != wantSel )
                    continue;
                float p[3];
                if ( !KiwiCon_AnchorWorld( o, a, p ) )
                    break;
                // ROUND U: 16, not 6 — one filled dot is 12 segments (see
                // DrawFilledDot), and a guard smaller than the glyph would let a
                // half-drawn dot into the batch's last slots.
                if ( KiwiLines_Remaining() < 16 )
                {
                    KiwiLines_Flush();
                    return;
                }
                DrawFilledDot( c, p, wantSel ? KCON_DOT_PIX_SEL : KCON_DOT_PIX );
            }
        }
    }

    KiwiLines_Flush();
}

// ─── settings ────────────────────────────────────────────────────────────────
// -- KIWI-UX (ROUND AF, ITEM 7): THE REMEMBERED SIDE COUNT -------------------
// One number for every round tool, persisted in kiwi_radiant.ini next to the exe
// (radiant_registry.h - no registry, ever).  ONE rather than one per tool on
// purpose: "I work at 32 sides" is a statement about the map being built, not
// about which of the four round tools drew a given ring, and four independent
// numbers is four places for the answer to be stale.  0 = never set = AUTO,
// which is what every pre-round-AF session is.
//
// Lazy-loaded on first read, exactly as KiwiCon_ShowConstruction below is and
// for the same reason (the profile path is not ready at static-init time).
static int s_toolSides = -1;      // -1 = not loaded yet

int KiwiCon_ToolSides()
{
    if ( s_toolSides < 0 )
    {
        int n = Radiant_ProfileGetInt( KCON_SECTION, "RoundToolSides", 0 );
        if ( n != 0 )
        {
            if ( n < KCON_SIDES_MIN ) n = KCON_SIDES_MIN;
            if ( n > KCON_SEGS_MAX  ) n = KCON_SEGS_MAX;
        }
        s_toolSides = n;
    }
    return s_toolSides;
}

void KiwiCon_SetToolSides( int sides )
{
    int n = sides;
    if ( n != 0 )
    {
        if ( n < KCON_SIDES_MIN ) n = KCON_SIDES_MIN;
        if ( n > KCON_SEGS_MAX  ) n = KCON_SEGS_MAX;
    }
    if ( n == s_toolSides )
        return;
    s_toolSides = n;
    Radiant_ProfileSetInt( KCON_SECTION, "RoundToolSides", n );
}

bool KiwiCon_ShowConstruction()
{
    if ( s_showConstruction < 0 )
        s_showConstruction = Radiant_ProfileGetInt( KCON_SECTION, "ShowConstruction", 1 ) ? 1 : 0;
    return s_showConstruction != 0;
}

void KiwiCon_SetShowConstruction( bool on )
{
    const int v = on ? 1 : 0;
    if ( s_showConstruction == v )
        return;
    s_showConstruction = v;
    Radiant_ProfileSetInt( KCON_SECTION, "ShowConstruction", v );
    g_nUpdateBits |= 1;
}

// ═════════════════════════════════════════════════════════════════════════════
//  §7 sidecar persistence.  Format sample at the top of this file.
// ═════════════════════════════════════════════════════════════════════════════
namespace
{
    // "<dir>/<name>.map" → "<dir>/<name>.kiwi".  A path with no extension simply
    // gets one appended; a path with a directory that has a dot in it and no file
    // extension is handled by only ever cutting AFTER the last separator.
    bool SidecarPath( const char *mapPath, char *out, int outSize )
    {
        if ( !mapPath || !mapPath[0] || outSize < 8 )
            return false;
        _snprintf( out, outSize, "%s", mapPath );
        out[outSize - 1] = '\0';

        char *lastSep = 0;
        for ( char *p = out; *p; ++p )
            if ( *p == '/' || *p == '\\' )
                lastSep = p;
        char *dot = 0;
        for ( char *p = ( lastSep ? lastSep + 1 : out ); *p; ++p )
            if ( *p == '.' )
                dot = p;
        if ( dot )
            *dot = '\0';

        const int len = (int)strlen( out );
        if ( len + 6 >= outSize )
            return false;
        strcat( out, ".kiwi" );
        return true;
    }

    const char *TypeName( kconType_t t )
    {
        switch ( t )
        {
        case KCON_LINE:     return "line";
        case KCON_POLYLINE: return "polyline";
        case KCON_RECT:     return "rect";
        case KCON_CIRCLE:   return "circle";
        case KCON_ARC:      return "arc";
        default:            return "line";
        }
    }

    bool TypeFromName( const char *s, kconType_t *out )
    {
        if ( !strcmp( s, "line" )     ) { *out = KCON_LINE;     return true; }
        if ( !strcmp( s, "polyline" ) ) { *out = KCON_POLYLINE; return true; }
        if ( !strcmp( s, "rect" )     ) { *out = KCON_RECT;     return true; }
        if ( !strcmp( s, "circle" )   ) { *out = KCON_CIRCLE;   return true; }
        if ( !strcmp( s, "arc" )      ) { *out = KCON_ARC;      return true; }
        return false;
    }
}

bool KiwiCon_SaveSidecar( const char *mapPath )
{
    char path[1100];
    if ( !SidecarPath( mapPath, path, sizeof( path ) ) )
        return false;

    // ROUND AO: the sidecar is no longer only about construction geometry — it also
    // carries which SOLIDS are hidden.  Build that enumeration FIRST, because it
    // decides (with s_objects) whether the file has any reason to exist at all.
    // See kiwi_visibility.h ROUND AO for the ordinal space and its limits.
    const int hiddenCount = KiwiVis_SidecarBuild();

    if ( s_objects.empty() && hiddenCount == 0 )
    {
        // A map that never used construction geometry and has nothing hidden never
        // grows a file — and one whose geometry was cleared and whose brushes were
        // all unhidden loses its stale sidecar rather than keeping it.
        ::DeleteFileA( path );
        return true;
    }

    // tmp + rename (§7): a crash or a full disk mid-write must not be able to
    // destroy the sidecar that was already there.
    char tmp[1120];
    _snprintf( tmp, sizeof( tmp ), "%s.tmp", path );
    tmp[sizeof( tmp ) - 1] = '\0';

    FILE *f = fopen( tmp, "wb" );
    if ( !f )
    {
        Sys_Printf( "WARNING: could not write construction sidecar %s\n", tmp );
        return false;
    }

    fprintf( f, "KIWI2\n" );
    fprintf( f, "# KIWI construction geometry (RADIANT_UX_DESIGN section 7).\n" );
    fprintf( f, "# Editor-only: nothing here is map data and nothing here is compiled.\n" );

    // ── ROUND AO: hidden SOLIDS ────────────────────────────────────────────────
    // Top-level lines, written FIRST and ONLY when something is hidden, so a map
    // with nothing hidden writes exactly the bytes every earlier build wrote.  The
    // format version deliberately stays KIWI2 on the same back-compat argument the
    // `hidden` / `group` / `name` keywords make below: an older reader falls
    // through its `if (!inObject) continue;` gate and skips both keywords in
    // silence, and this build treats their absence as "nothing was hidden".
    //
    // `hiddenbrushtotal` is the size of the whole enumeration, not the hidden
    // count; it is the staleness guard — see KiwiVis_SidecarLoadApply.  The
    // ordinals are indices into Map_SaveFile's own brush walk (map.cpp:693-706 +
    // :1515-1517), which is FRAGILE BY CONSTRUCTION: edit the .map outside KIWI and
    // every ordinal past the edit means a different brush.  That is why the guard
    // exists, why an out-of-range ordinal is dropped rather than clamped, and why
    // nothing here is ever an error — a wrong hide is cosmetic and Alt+H clears it.
    if ( hiddenCount > 0 )
    {
        fprintf( f, "# Hidden solids (editor-only).  Ordinals index the .map save order;\n" );
        fprintf( f, "# they are only valid for the .map written beside this file.\n" );
        fprintf( f, "hiddenbrushtotal %i\n", KiwiVis_SidecarTotal() );
        for ( int i = 0; i < hiddenCount; ++i )
        {
            const int ord = KiwiVis_SidecarOrdinal( i );
            if ( ord >= 0 )
                fprintf( f, "hiddenbrush %i\n", ord );
        }
    }

    // ROUND W: the group DECLARATIONS, before any object, so the reader has the
    // whole table by the time it meets the first `group N`.  Written even for a
    // group with no members — an empty folder is a legal drag target and losing it
    // on save would make the outliner forget a folder the user just made.  Quotes
    // around the name so it may contain spaces; the reader takes the rest of the
    // line between the first and last quote, so a name may not contain a quote.
    for ( size_t g = 0; g < s_groups.size(); ++g )
        fprintf( f, "congroup %i \"%s\"\n", s_groups[g].id, s_groups[g].name );
    for ( size_t i = 0; i < s_objects.size(); ++i )
    {
        const kconObject_t &o = s_objects[i];
        fprintf( f, "object %s\n", TypeName( o.type ) );
        // SHAKEOUT H: the plane is written ONLY for the parametric types, where it
        // is authoritative.  A point object's plane is derived from its own points
        // on load (KiwiCon_Add refits), so writing it would be storing a cache in a
        // file — a second source of truth that could disagree with the first.
        if ( KiwiCon_IsParametric( o ) )
        {
            fprintf( f, "plane %g %g %g  %g %g %g  %g %g %g  %g %g %g\n",
                     (double)o.plane.origin[0], (double)o.plane.origin[1], (double)o.plane.origin[2],
                     (double)o.plane.normal[0], (double)o.plane.normal[1], (double)o.plane.normal[2],
                     (double)o.plane.u[0],      (double)o.plane.u[1],      (double)o.plane.u[2],
                     (double)o.plane.v[0],      (double)o.plane.v[1],      (double)o.plane.v[2] );
        }
        fprintf( f, "closed %i\n", o.closed ? 1 : 0 );
        // ROUND U: written ONLY when true, and the format version stays KIWI2.
        // Back-compat runs BOTH ways by construction: an older build's reader
        // skips every keyword it does not know ("Any other keyword is a newer
        // file's addition — skipped silently", the load loop's tail), and this
        // build's reader defaults `hidden` to false when the line is absent —
        // which is exactly what every KIWI2 file written before this round is.
        // A version bump would have refused those files for one bool.
        if ( o.hidden )
            fprintf( f, "hidden 1\n" );
        // ROUND W: written ONLY when grouped, on the same back-compat argument the
        // `hidden` line above makes.  A group id that survived into the object but
        // whose declaration did not (a hand-edited file) is dropped by the reader.
        if ( o.group >= 0 )
            fprintf( f, "group %i\n", o.group );
        // ROUND X, ITEM 10: the outliner name, written ONLY when set — the same
        // optional-keyword, no-version-bump deal `hidden` and `group` make above.
        // Quoted like `congroup`, and read back the same way (between the first and
        // the last quote), so a name may hold spaces but not a quote;
        // KiwiCon_SetName strips quotes at the source so one can never get here.
        if ( !o.name.empty() )
            fprintf( f, "name \"%s\"\n", o.name.c_str() );
        if ( KiwiCon_IsParametric( o ) )
        {
            // ROUND AF, ITEM 7: omitted when AUTO, so a store that never touched
            // the field writes exactly the bytes it wrote before this round.
            if ( o.segs > 0 )
                fprintf( f, "segs %i\n", o.segs );
            fprintf( f, "arc %g %g %g %g %g\n",
                     (double)o.centre[0], (double)o.centre[1], (double)o.radius,
                     (double)o.ang0, (double)o.ang1 );
        }
        else
        {
            for ( size_t k = 0; k + 2 < o.pts.size(); k += 3 )
                fprintf( f, "wpt %g %g %g\n",
                         (double)o.pts[k], (double)o.pts[k + 1], (double)o.pts[k + 2] );
        }
        fprintf( f, "end\n" );
    }
    fclose( f );

    if ( !::MoveFileExA( tmp, path, MOVEFILE_REPLACE_EXISTING ) )
    {
        Sys_Printf( "WARNING: could not replace construction sidecar %s\n", path );
        ::DeleteFileA( tmp );
        return false;
    }
    // ROUND AO: the readout names both payloads, because "Saved 0 construction
    // object(s)" on a map that only had hidden brushes reads like a bug.
    if ( hiddenCount > 0 )
        Sys_Printf( "Saved %i construction object(s) and %i hidden brush(es) to %s\n",
                    (int)s_objects.size(), hiddenCount, path );
    else
        Sys_Printf( "Saved %i construction object(s) to %s\n", (int)s_objects.size(), path );
    return true;
}

bool KiwiCon_LoadSidecar( const char *mapPath )
{
    s_objects.clear();
    // ROUND W: the group table is part of the store, so a map load replaces it too.
    s_groups.clear();
    s_nextGroupId = 1;
    s_undo.clear();
    s_redo.clear();               // shakeout I
    // SHAKEOUT I: a map load resets BOTH stores, so the unified journal's tickets
    // (kiwi_undo.h) go with them.  The legacy half is already dropped by
    // Undo_Clear's own hook; this is the construction half, and doing it here —
    // the one function a map load always runs — keeps the two resets together.
    KiwiUndo_Reset();
    // ROUND AO: drop any hidden-brush ordinals left pending from a previous load.
    // At the TOP, ahead of every `return false` below, so a missing sidecar, an
    // unreadable one, or one with no `hiddenbrush` lines at all reliably leaves the
    // pending set EMPTY — otherwise loading map B after map A would hide brushes in
    // B at A's ordinals, which is the worst failure this feature could have.
    KiwiVis_SidecarLoadBegin();
    Touch();

    char path[1100];
    if ( !SidecarPath( mapPath, path, sizeof( path ) ) )
        return false;

    FILE *f = fopen( path, "rb" );
    if ( !f )
        return false;                    // no sidecar is the normal case, not an error

    char line[512];
    // SHAKEOUT H: BOTH versions load.  KIWI1 is the plane-space format ruling 3
    // used to write (`plane` + `pt u v`); KIWI2 is the world-space one it writes
    // now (`wpt x y z`, `plane` only for circles and arcs).  Refusing a KIWI1 file
    // would silently throw away every piece of scaffolding a user has ever drawn,
    // and the conversion is four lines — see the `pt` and `end` arms below.
    if ( !fgets( line, sizeof( line ), f )
      || ( strncmp( line, "KIWI1", 5 ) != 0 && strncmp( line, "KIWI2", 5 ) != 0 ) )
    {
        fclose( f );
        Sys_Printf( "WARNING: %s is not a KIWI construction sidecar — ignored.\n", path );
        return false;
    }

    std::vector<kconObject_t> loaded;
    kconObject_t cur;
    std::vector<float> legacyUV;      // KIWI1 `pt` points, converted at `end`
    bool inObject = false;
    bool bad      = false;

    while ( fgets( line, sizeof( line ), f ) )
    {
        char kw[32] = { 0 };
        if ( sscanf( line, "%31s", kw ) != 1 )
            continue;
        if ( kw[0] == '#' )
            continue;

        if ( !strcmp( kw, "object" ) )
        {
            char tn[32] = { 0 };
            cur = kconObject_t();
            legacyUV.clear();
            inObject = true;
            if ( sscanf( line, "%*s %31s", tn ) != 1 || !TypeFromName( tn, &cur.type ) )
            {
                // Unknown TYPE: keep parsing so a newer file's extra object kinds
                // do not cost the user the objects this build does understand.
                inObject = false;
                bad      = true;
            }
            continue;
        }
        // ROUND W: the group DECLARATIONS.  Parsed BEFORE the `!inObject` gate,
        // because they are top-level lines — an older build reaches that gate and
        // skips them, which is exactly the back-compat behaviour wanted.
        // `congroup <id> "<name>"`; the name is what lies between the first and the
        // last quote, so a name may contain spaces but not a quote.
        if ( !strcmp( kw, "congroup" ) )
        {
            int gid = -1;
            if ( sscanf( line, "%*s %i", &gid ) == 1 && gid >= 0 )
            {
                char        nm[KCON_GROUPNAME_MAX] = { 0 };
                const char *q0 = strchr( line, '"' );
                const char *q1 = q0 ? strrchr( line, '"' ) : 0;
                if ( q0 && q1 && q1 > q0 )
                {
                    int n = (int)( q1 - q0 - 1 );
                    if ( n > KCON_GROUPNAME_MAX - 1 )
                        n = KCON_GROUPNAME_MAX - 1;
                    memcpy( nm, q0 + 1, (size_t)n );
                    nm[n] = '\0';
                }
                if ( GroupSlot( gid ) < 0 )
                {
                    kconGroup_t g;
                    memset( &g, 0, sizeof( g ) );
                    g.id = gid;
                    if ( nm[0] )
                        _snprintf( g.name, sizeof( g.name ), "%s", nm );
                    else
                        _snprintf( g.name, sizeof( g.name ), "Group %i", gid );
                    g.name[sizeof( g.name ) - 1] = '\0';
                    s_groups.push_back( g );
                    // Keep minting ABOVE anything the file declared, so a group made
                    // after the load can never collide with one that came from it.
                    if ( gid >= s_nextGroupId )
                        s_nextGroupId = gid + 1;
                }
            }
            else bad = true;
            continue;
        }
        // ROUND AO: the hidden-SOLID records.  Top-level lines like `congroup`, so
        // they are parsed BEFORE the `!inObject` gate — which is also exactly where
        // an older build skips them.  A malformed line is IGNORED and does NOT set
        // `bad`: `bad` makes the reader warn about lost construction OBJECTS, and a
        // visibility record that did not parse has cost the user no geometry.
        if ( !strcmp( kw, "hiddenbrushtotal" ) )
        {
            int n = -1;
            if ( sscanf( line, "%*s %i", &n ) == 1 && n >= 0 )
                KiwiVis_SidecarLoadTotal( n );
            continue;
        }
        if ( !strcmp( kw, "hiddenbrush" ) )
        {
            int n = -1;
            if ( sscanf( line, "%*s %i", &n ) == 1 )
                KiwiVis_SidecarLoadNote( n );      // range-checked at apply time
            continue;
        }
        if ( !inObject )
            continue;

        if ( !strcmp( kw, "plane" ) )
        {
            float o[3], n[3], u[3], v[3];
            if ( sscanf( line, "%*s %f %f %f %f %f %f %f %f %f %f %f %f",
                         &o[0], &o[1], &o[2], &n[0], &n[1], &n[2],
                         &u[0], &u[1], &u[2], &v[0], &v[1], &v[2] ) == 12 )
            {
                // Rebuilt through MakePlane rather than trusted verbatim: a
                // hand-edited or float-rounded basis must not be able to make the
                // rest of the layer solve on a non-orthonormal frame.
                if ( !KiwiCon_MakePlane( o, n, u, &cur.plane ) )
                    bad = true;
            }
            else bad = true;
        }
        else if ( !strcmp( kw, "closed" ) )
        {
            int c = 0;
            if ( sscanf( line, "%*s %i", &c ) == 1 ) cur.closed = ( c != 0 );
            else                                     bad = true;
        }
        else if ( !strcmp( kw, "hidden" ) )              // ROUND U — absent = visible
        {
            int h = 0;
            if ( sscanf( line, "%*s %i", &h ) == 1 ) cur.hidden = ( h != 0 );
            else                                     bad = true;
        }
        else if ( !strcmp( kw, "group" ) )              // ROUND W — absent = ungrouped
        {
            int g = -1;
            if ( sscanf( line, "%*s %i", &g ) == 1 )
            {
                // An id with no `congroup` declaration is not a group.  Dropping it
                // to ungrouped is the only answer that cannot produce a folder the
                // name table has never heard of (see KiwiCon_SetGroup's same rule).
                cur.group = ( GroupSlot( g ) >= 0 ) ? g : -1;
            }
            else bad = true;
        }
        else if ( !strcmp( kw, "name" ) )               // ROUND X — absent = unnamed
        {
            // Between the first and the last quote, exactly as `congroup` reads its
            // own name.  A `name` line with no quotes at all is a malformed line and
            // simply leaves the object unnamed rather than failing the load — a
            // label is not worth refusing a user's scaffolding over.
            const char *q0 = strchr( line, '"' );
            const char *q1 = q0 ? strrchr( line, '"' ) : 0;
            if ( q0 && q1 && q1 > q0 )
            {
                int n = (int)( q1 - q0 - 1 );
                if ( n > KCON_NAME_MAX - 1 )
                    n = KCON_NAME_MAX - 1;
                cur.name.assign( q0 + 1, (size_t)n );
            }
        }
        else if ( !strcmp( kw, "wpt" ) )                 // KIWI2 — WORLD, verbatim
        {
            float a, b, c;
            if ( sscanf( line, "%*s %f %f %f", &a, &b, &c ) == 3 )
            {
                if ( (int)( cur.pts.size() / 3 ) < KCON_MAX_POINTS )
                {
                    cur.pts.push_back( a );
                    cur.pts.push_back( b );
                    cur.pts.push_back( c );
                }
                else bad = true;
            }
            else bad = true;
        }
        else if ( !strcmp( kw, "pt" ) )                  // KIWI1 — plane space
        {
            float a, b;
            if ( sscanf( line, "%*s %f %f", &a, &b ) == 2 )
            {
                if ( (int)( legacyUV.size() / 2 ) < KCON_MAX_POINTS )
                {
                    legacyUV.push_back( a );
                    legacyUV.push_back( b );
                }
                else bad = true;
            }
            else bad = true;
        }
        else if ( !strcmp( kw, "segs" ) )
        {
            // ROUND AF, ITEM 7.  Clamped on the way IN as well as on the way out —
            // a hand-edited sidecar is a text file and this is the only place that
            // can stop a 4000-sided circle reaching the tessellator.
            int n = 0;
            if ( sscanf( line, "%*s %i", &n ) == 1 && n > 0 )
            {
                if ( n < KCON_SIDES_MIN ) n = KCON_SIDES_MIN;
                if ( n > KCON_SEGS_MAX  ) n = KCON_SEGS_MAX;
                cur.segs = n;
            }
        }
        else if ( !strcmp( kw, "arc" ) )
        {
            float cu, cv, r, a0, a1;
            if ( sscanf( line, "%*s %f %f %f %f %f", &cu, &cv, &r, &a0, &a1 ) == 5 )
            {
                cur.centre[0] = cu; cur.centre[1] = cv;
                cur.radius = r; cur.ang0 = a0; cur.ang1 = a1;
            }
            else bad = true;
        }
        else if ( !strcmp( kw, "end" ) )
        {
            inObject = false;
            // KIWI1 -> KIWI2: the plane-space points become world through the
            // object's own `plane`, which the v1 writer always emitted just above
            // them.  A file that gave `pt` without a `plane` gets the default XY
            // basis, which is what kconPlane_t default-constructs to — the same
            // answer the v1 code would have produced for that file.
            if ( !legacyUV.empty() )
            {
                for ( size_t k = 0; k + 1 < legacyUV.size(); k += 2 )
                {
                    float w[3];
                    KiwiCon_PlaneToWorld( cur.plane, &legacyUV[k], w );
                    cur.pts.push_back( w[0] );
                    cur.pts.push_back( w[1] );
                    cur.pts.push_back( w[2] );
                }
                legacyUV.clear();
            }
            // KIWI-UX (ROUND R): the SAME seam rule KiwiCon_Add enforces (see the
            // note there).  A sidecar written by any earlier build — or by hand —
            // can carry a repeated closing vertex, and a store loaded straight off
            // disk bypasses KiwiCon_Add entirely (the vector is swapped in below),
            // so without this the fixed producers would still be undone by an old
            // file.  Cheap and idempotent: an already-clean object is unchanged.
            NormalizePoints( cur );
            if ( KiwiCon_IsParametric( cur ) ? ( cur.radius > 1.0e-3f )
                                             : ( (int)( cur.pts.size() / 3 ) >= 2 ) )
                loaded.push_back( cur );
            else
                bad = true;
        }
        // Any other keyword is a newer file's addition — skipped silently.
    }
    fclose( f );

    s_objects.swap( loaded );
    s_undo.clear();
    s_redo.clear();               // shakeout I
    RefitAll();                  // shakeout H: every point object's cached plane
    Touch();

    if ( bad )
        Sys_Printf( "WARNING: %s had unreadable entries — %i object(s) loaded.\n",
                    path, (int)s_objects.size() );
    else if ( !s_objects.empty() )
        Sys_Printf( "Loaded %i construction object(s) from %s\n", (int)s_objects.size(), path );
    return true;
}

// ─── commands ────────────────────────────────────────────────────────────────
bool KiwiCon_CanDraw()
{
    return true;                          // a drawing tool always has a plane to draw on
}

void KiwiCon_RegisterCommands()
{
    // Unbound: these are the CLASSIC-profile bindings, and the modern profile
    // deliberately claims NO new key this phase (see the KEYS note in the header).
    Radiant_RegisterCommand( "KiwiConstructLine",      0, 0, KIWI_CMD_DRAW_LINE );
    Radiant_RegisterCommand( "KiwiConstructPolyline",  0, 0, KIWI_CMD_DRAW_POLYLINE );
    Radiant_RegisterCommand( "KiwiConstructRect",      0, 0, KIWI_CMD_DRAW_RECT );
    Radiant_RegisterCommand( "KiwiConstructCircle",    0, 0, KIWI_CMD_DRAW_CIRCLE );
    Radiant_RegisterCommand( "KiwiConstructArc",       0, 0, KIWI_CMD_DRAW_ARC );
    // §16b shakeout C — the rest of the curve inventory.  Also unbound: the ONE
    // key this round claims is Shift+A for the add menu (kiwi_addmenu.cpp), which
    // is how all nine of these are reached in one keystroke.
    Radiant_RegisterCommand( "KiwiConstructRectCenter", 0, 0, KIWI_CMD_DRAW_RECT_CENTER );
    Radiant_RegisterCommand( "KiwiConstructCircle2Pt",  0, 0, KIWI_CMD_DRAW_CIRCLE_2PT );
    Radiant_RegisterCommand( "KiwiConstructPolygon",    0, 0, KIWI_CMD_DRAW_POLYGON );
    Radiant_RegisterCommand( "KiwiConstructSpline",     0, 0, KIWI_CMD_DRAW_SPLINE );
    Radiant_RegisterCommand( "KiwiConstructPlaneXY",   0, 0, KIWI_CMD_CPLANE_XY );
    Radiant_RegisterCommand( "KiwiConstructPlaneXZ",   0, 0, KIWI_CMD_CPLANE_XZ );
    Radiant_RegisterCommand( "KiwiConstructPlaneYZ",   0, 0, KIWI_CMD_CPLANE_YZ );
    Radiant_RegisterCommand( "KiwiConstructPlaneFace", 0, 0, KIWI_CMD_CPLANE_FACE );
    Radiant_RegisterCommand( "KiwiConstructPlaneView", 0, 0, KIWI_CMD_CPLANE_VIEW );
    Radiant_RegisterCommand( "KiwiConstructClear",     0, 0, KIWI_CMD_CONSTRUCT_CLEAR );
    Radiant_RegisterCommand( "KiwiConstructUndo",      0, 0, KIWI_CMD_CONSTRUCT_UNDO );
}

KiwiEditorCommand *KiwiCon_CommandForId( int commandId )
{
    switch ( commandId )
    {
    // ROUND P: THE DEDUP.  Line and Polyline are the same tool now (KiwiCurveTool's
    // header note carries the Plasticity cites and the grammar).  Both ids stay
    // registered so no keymap row, menu row or palette entry naming either one goes
    // dead — they simply resolve to one object.
    case KIWI_CMD_DRAW_LINE:     return &s_curveTool;
    case KIWI_CMD_DRAW_POLYLINE: return &s_curveTool;
    case KIWI_CMD_DRAW_RECT:     return &s_rectTool;
    case KIWI_CMD_DRAW_CIRCLE:   return &s_circleTool;
    case KIWI_CMD_DRAW_ARC:      return &s_arcTool;
    case KIWI_CMD_DRAW_RECT_CENTER: return &s_rectCenterTool;
    case KIWI_CMD_DRAW_CIRCLE_2PT:  return &s_circle2PtTool;
    case KIWI_CMD_DRAW_POLYGON:     return &s_polygonTool;
    case KIWI_CMD_DRAW_SPLINE:      return &s_splineTool;
    default:                     return 0;
    }
}

bool KiwiCon_DispatchInstant( unsigned int commandId )
{
    switch ( commandId )
    {
    case KIWI_CMD_CPLANE_XY:   KiwiCon_SetPlaneAxis( 2 );  return true;
    case KIWI_CMD_CPLANE_XZ:   KiwiCon_SetPlaneAxis( 1 );  return true;
    case KIWI_CMD_CPLANE_YZ:   KiwiCon_SetPlaneAxis( 0 );  return true;
    case KIWI_CMD_CPLANE_VIEW: KiwiCon_SetPlaneFromView(); return true;
    case KIWI_CMD_CPLANE_FACE:
        if ( KiwiCon_SetPlaneFromCursorFace() )
            Sys_Printf( "Construction plane: from face under cursor.\n" );
        else
            Sys_Printf( "Construction plane: no face under the cursor.\n" );
        return true;
    case KIWI_CMD_CONSTRUCT_CLEAR:
        if ( KiwiCon_HasObjects() )
        {
            KiwiCon_UndoPush();
            KiwiCon_ClearAll();
            Sys_Printf( "Construction: cleared.\n" );
        }
        return true;
    case KIWI_CMD_CONSTRUCT_UNDO:
        // SHAKEOUT I: this command is now an ALIAS for the one Ctrl+Z, for the same
        // reason the drawing tools' own Ctrl+Z is.  Popping the construction stack
        // behind the journal's back would leave a ticket naming a snapshot that had
        // already been consumed, and the journal would then forward the NEXT Ctrl+Z
        // to a stack one step shorter than it believed.  One driver, one order.
        if ( !KiwiUndo_Undo() )
            Sys_Printf( "Nothing left to undo.\n" );
        return true;
    default:
        return false;
    }
}

// ─── the "Construct" block in the shell's panel window ───────────────────────
// NOT an ImGui menu: ImGuiPanels_Menu draws inside an ordinary ImGui::Begin
// window, so BeginMenu/MenuItem would be illegal there.  Same shape as
// KiwiPalette_MenuItem — buttons, in a labelled section.  §11 requires every new
// feature to be reachable in BOTH keymap profiles, and since nothing here binds a
// key this phase, this block and the command palette ARE the route.
void KiwiCon_MenuItems()
{
    extern void Radiant_ExecCommand( unsigned int cmdId );   // mainfrm.cpp

    ImGui::SeparatorText( "Construct" );

    struct row_t { const char *label; int id; };
    static const row_t KTOOLS[9] =
    {
        // ROUND P: the two are one tool now (KiwiCurveTool); the second button is
        // kept as a discoverable alias rather than deleted.
        { "Line",       KIWI_CMD_DRAW_LINE        },
        { "Poly(=Line)",KIWI_CMD_DRAW_POLYLINE    },
        { "Spline",     KIWI_CMD_DRAW_SPLINE      },
        { "Rect",       KIWI_CMD_DRAW_RECT        },
        { "Rect(ctr)",  KIWI_CMD_DRAW_RECT_CENTER },
        { "Circle",     KIWI_CMD_DRAW_CIRCLE      },
        { "Circle(2p)", KIWI_CMD_DRAW_CIRCLE_2PT  },
        { "Arc",        KIWI_CMD_DRAW_ARC         },
        { "Polygon",    KIWI_CMD_DRAW_POLYGON     },
    };
    for ( int i = 0; i < 9; ++i )
    {
        if ( i % 3 )                              // 3 per row — nine buttons on one
            ImGui::SameLine();                    // line would run off the panel
        if ( ImGui::Button( KTOOLS[i].label ) )
            Radiant_ExecCommand( (unsigned int)KTOOLS[i].id );
    }

    // SHAKEOUT H: Trim sits with the drawing tools because it is a LINE verb, and
    // §11 requires every new feature to be reachable in BOTH keymap profiles — the
    // classic profile does not bind T.
    ImGui::BeginDisabled( !KiwiTrim_CanTrim() );
    if ( ImGui::Button( "Trim Lines (T)" ) )
        Radiant_ExecCommand( KIWI_CMD_TRIM );
    ImGui::EndDisabled();
    // SHAKEOUT H FIX: ImGuiHoveredFlags_AllowWhenDisabled.  IsItemHovered() returns
    // FALSE on a disabled item by default (deps/imgui/imgui.h:1025), so every one of
    // the "why is this greyed out" tooltips in this block was DEAD CODE — it could
    // only fire while the button was ENABLED, and the predicate beside it is false
    // exactly then.  All three in this function are fixed the same way.
    if ( ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) && !KiwiTrim_CanTrim() )
        ImGui::SetTooltip( "Draw a line or a polyline first.\n"
                           "Trim removes the piece between two crossings." );
    ImGui::SameLine();

    const bool canExtrude = !KiwiRegion_All().empty();
    ImGui::BeginDisabled( !canExtrude );
    if ( ImGui::Button( "Extrude Region" ) )
        Radiant_ExecCommand( KIWI_CMD_EXTRUDE_REGION );
    ImGui::EndDisabled();
    if ( ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) && !canExtrude )
        ImGui::SetTooltip( "Draw a closed loop first (a rect, a circle,\n"
                           "or a polyline clicked back onto its first point)." );

    // ── ROUND J: the two curve EDITORS, next to the curve TOOLS ─────────────
    // Same reasoning as Trim above: they are line verbs, and §11 requires every
    // feature to be reachable in BOTH keymap profiles (classic binds neither O
    // nor B).  Both act on the CONSTRUCTION SELECTION, so both are greyed until
    // one exists — with the tooltip saying how to get one.
    const bool canOffset = KiwiOffset_CanOffset();
    ImGui::BeginDisabled( !canOffset );
    if ( ImGui::Button( "Offset (O)" ) )
        Radiant_ExecCommand( KIWI_CMD_OFFSET_CURVE );
    ImGui::EndDisabled();
    if ( ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) && !canOffset )
        ImGui::SetTooltip( "Select a construction line, polyline, rect,\n"
                           "circle or arc first (click it in any mode).\n"
                           "Offset makes a parallel COPY; the original stays." );
    ImGui::SameLine();

    const bool canFillet = KiwiFillet_CanFillet();
    ImGui::BeginDisabled( !canFillet );
    if ( ImGui::Button( "Fillet (B)" ) )
    {
        // KIWI-UX (ROUND Q): KiwiCmd_Start, NOT Radiant_ExecCommand.  Bare B is now
        // a context verb — KiwiCmd_DispatchInner redirects KIWI_CMD_FILLET_CURVE to
        // the SOLID fillet whenever brush edges are selected (kiwi_patchfillet.h
        // THE B KEY), and the two selections are parallel stores that can both be
        // non-empty.  This button lives in the CONSTRUCTION panel and is labelled
        // for the construction fillet, so it must mean that one unconditionally.
        // Starting the modal directly is the palette's own route for a KIWI id and
        // skips only the Repeat-Last recording, which the key already provides.
        KiwiCmd_Start( KIWI_CMD_FILLET_CURVE );
    }
    ImGui::EndDisabled();
    if ( ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) && !canFillet )
        ImGui::SetTooltip( "Select a planar polyline or rect with corners.\n"
                           "Every corner rounds; select individual anchors\n"
                           "in Point mode (1) to round only those." );

    static const row_t KPLANES[5] =
    {
        { "XY",        KIWI_CMD_CPLANE_XY   },
        { "XZ",        KIWI_CMD_CPLANE_XZ   },
        { "YZ",        KIWI_CMD_CPLANE_YZ   },
        { "From face", KIWI_CMD_CPLANE_FACE },
        { "From view", KIWI_CMD_CPLANE_VIEW },
    };
    ImGui::TextDisabled( "Construction plane" );
    for ( int i = 0; i < 5; ++i )
    {
        if ( i )
            ImGui::SameLine();
        ImGui::PushID( 900 + i );
        if ( ImGui::Button( KPLANES[i].label ) )
            Radiant_ExecCommand( (unsigned int)KPLANES[i].id );
        ImGui::PopID();
    }

    // SHAKEOUT F: the construction SELECTION's two verbs, so they are reachable in
    // the CLASSIC profile too (§11: every new feature works in both).
    ImGui::TextDisabled( "Selection (%i item(s))", KiwiConSel_Count() );
    ImGui::BeginDisabled( !KiwiConSel_CanJoin() );
    if ( ImGui::Button( "Join Lines" ) )
        Radiant_ExecCommand( KIWI_CMD_CONSTRUCT_JOIN );
    ImGui::EndDisabled();
    if ( ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) && !KiwiConSel_CanJoin() )
        ImGui::SetTooltip( "Select two or more open construction lines whose\n"
                           "ends meet (click them, or drag a marquee)." );
    ImGui::SameLine();
    ImGui::BeginDisabled( !KiwiConSel_CanDelete() );
    if ( ImGui::Button( "Delete Selected" ) )
        Radiant_ExecCommand( KIWI_CMD_CONSTRUCT_DELETE );
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled( KiwiConSel_Empty() );
    if ( ImGui::Button( "Deselect" ) )
        KiwiConSel_Clear();
    ImGui::EndDisabled();

    // SHAKEOUT I: the button is greyed on the JOURNAL's depth, not the store's —
    // the command it posts is the unified undo now, so the store's own depth would
    // grey a button that has plenty to do (and enable one that does not).
    ImGui::BeginDisabled( KiwiUndo_UndoDepth() <= 0 );
    if ( ImGui::Button( "Undo (unified)" ) )
        Radiant_ExecCommand( KIWI_CMD_CONSTRUCT_UNDO );
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled( !KiwiCon_HasObjects() );
    if ( ImGui::Button( "Clear all" ) )
        Radiant_ExecCommand( KIWI_CMD_CONSTRUCT_CLEAR );
    ImGui::EndDisabled();

    bool show = KiwiCon_ShowConstruction();
    if ( ImGui::Checkbox( "Show construction geometry", &show ) )
        KiwiCon_SetShowConstruction( show );

    ImGui::TextDisabled( "%i object(s), %i region(s), %i construction snapshot(s)",
                         KiwiCon_Count(), (int)KiwiRegion_All().size(), KiwiCon_UndoDepth() );
    // SHAKEOUT I: the unified timeline's own readout, so the split that used to be
    // invisible is now the thing you can actually see.
    ImGui::TextDisabled( "timeline: %i undo / %i redo%s%s",
                         KiwiUndo_UndoDepth(), KiwiUndo_RedoDepth(),
                         KiwiUndo_UndoLabel() ? "   next: " : "",
                         KiwiUndo_UndoLabel() ? KiwiUndo_UndoLabel() : "" );
}

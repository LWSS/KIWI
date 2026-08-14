#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_snap.cpp — RADIANT_UX_DESIGN §6 SnapManager v3.  See kiwi_snap.h for the
// exact resolution rule this implements, the deviation note on the vertex radius
// and the construction-candidate budget.
//
// NEW code over the ported cores.  The BRUSH candidate walk is Pick()'s
// (kiwi_pick.cpp), so snapping can never disagree with picking/hovering; the
// CONSTRUCTION candidates are walked here because construction geometry is
// deliberately outside Pick (kiwi_construct.h scope ruling 1).  Nothing here
// mutates map data.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"        // camera_s
#include "prefs.h"          // g_PrefsDlg (camera_fov — the screen-scaled marker)
#include <imgui/imgui.h>

#include "kiwi_camera.h"    // ROUND M: KiwiCam_WorldPerPixel (the shared, ortho-aware screen-scale)
#include "kiwi_command.h"   // ROUND N: the active command + its cursor (the face accents)
#include "kiwi_snap.h"
#include "kiwi_conselect.h"   // ROUND AP, ITEM 2 — KiwiConSel_SnapMuted (self-snap)
#include "kiwi_construct.h"
#include "kiwi_transform.h"   // ROUND AP, ITEM 2 — KiwiXform_PivotPlacing (the one
                              // exception the self-snap exclusion makes, kiwi_transform.h:334)
#include "kiwi_grid.h"
#include "kiwi_region.h"      // KREG_JOIN_DIST — the ONE weld tolerance (shakeout F)
#include "kiwi_lines.h"
#include "kiwi_units.h"
#include "radiant_registry.h"

#include <math.h>
#include <stdio.h>

// ── ported entry points (verified against their definitions) ────────────────
extern camera_s *Ed_Camera();             // camwnd.cpp
// The ACTIVE display-list sentinel (0x23F189C): defined engine_stubs.cpp:698
// (`selbrush_t active_brushes{};`), same declaration kiwi_boxselect.cpp:41 and
// camwnd.cpp:28 carry.  `selected_brushes` is declared in qe3.h:1054 and defined
// engine_stubs.cpp:697.  READ ONLY here: the shakeout-F relative-angle arm walks
// brush edges near the drawing tool's anchor and mutates nothing.
extern selbrush_t active_brushes;

namespace
{
    const char *KSNAP_SECTION = "KiwiUX";
    int         s_showMarkers = -1;        // -1 = not loaded yet

    // ── MARKER COLOUR (shakeout E) ──────────────────────────────────────────
    // USER DIRECTIVE: "The tool previewer does not need to be a pink square.  Have
    // it just be a black dot with a black circle around the dot (with space
    // between like in plasticity)".
    //
    // So the marker is now ONE near-black colour for every type.  Type
    // differentiation moved entirely into the LABEL, which already carried a
    // per-rank tint (KiwiSnap_DrawLabel below, unchanged) and which is the only
    // place a colour can say "vertex" without also competing with the geometry it
    // is sitting on.  The five per-rank marker colours are gone with it: green on
    // green world geometry, amber on the grid and rose on the construction lines
    // themselves were all cases where the marker was the LEAST readable thing on
    // screen at the exact moment it mattered most.
    //
    // NOT PURE BLACK: kiwi_lines has no alpha channel (KiwiLines_Color takes rgb
    // only), so "near-black with slight alpha" is approximated with a very dark
    // grey — pure black reads as a hole punched in dark geometry, this reads as
    // ink on top of it.  The construction ROSE is untouched for construction
    // GEOMETRY (kiwi_construct.cpp); only the marker changed.
    const float KSNAP_COL_MARK[3] = { 0.05f, 0.05f, 0.07f };

    // Plasticity's point glyph, in pixels: a small filled dot, a clear gap of
    // nothing, then a separated ring.  Screen-constant, like every other size in
    // this layer.
    const float KSNAP_DOT_PIX   = 2.0f;
    const float KSNAP_RING_PIX  = 6.0f;

    // ── ROUND N: the hovered-face ACCENTS (KiwiSnap_DrawFaceAccents) ────────
    // Smaller than the marker's dot and with no ring, so "this is a place you
    // COULD snap" never reads as "this is what you HAVE snapped to".  Six sides is
    // enough for a 1.5 px disc and keeps the per-dot cost at nine segments (six
    // outline + three long diagonals — EmitDisc's solid form).
    const float KSNAP_ACCENT_PIX  = 1.5f;
    const int   KSNAP_ACCENT_SEGS = 6;
    const int   KSNAP_ACCENT_MAX  = 40;    // the directive's own budget
    const int   KSNAP_DOT_SEGS  = 6;       // hexagon — reads as solid at 2 px
    const int   KSNAP_RING_SEGS = 12;      // 12 chords read as round at 6 px

    int   s_labelX = 0;
    int   s_labelY = 0;

    // The world-space direction of the snapped edge, latched by the query so the
    // marker can draw its tick ALONG the edge.  Zero when the snap is not an edge
    // snap (the marker falls back to a camera-facing tick then).
    float s_edgeDir[3] = { 0.0f, 0.0f, 0.0f };
    bool  s_haveEdgeDir = false;

    // ── ROUND P: the AXIS arm (4c) — see kiwi_snap.h ────────────────────────
    const float KSNAP_AXIS_PIXELS      = 10.0f;   // base capture radius
    // ── KIWI-UX (ROUND X, ITEM 2): the FLAT-CAMERA radius, for EVERY axis ────
    // ROUND P widened only the VERTICAL guide (10 -> 16 px) as the camera dropped
    // toward the horizon.  The directive's shallow-angle case is exactly the flat
    // camera, and at a grazing pitch it is the IN-PLANE guides that are hardest to
    // land on too — a guide running away toward the horizon converges into a few
    // pixels of screen, so a 10 px capture is a couple of pixels of usable aim.
    //
    // 30 px is PLASTICITY'S OWN NUMBER for a line-shaped snapper: their raycaster
    // params carry `Line2: { threshold: 30 }`
    // (plasticity/src/editor/snaps/SnapPicker.ts:28-32, and again in
    // plasticity/src/command/point-picker/PointPicker.ts:138-142), and AxisSnap's
    // snapper IS a Line2 (plasticity/src/editor/snaps/AxisSnap.ts:25).  So the flat
    // end of the ramp is now Plasticity's threshold and the steep end keeps KIWI's
    // tighter 10 px, where a 30 px axis would start stealing clicks from geometry.
    const float KSNAP_AXIS_PIXELS_FLAT = 30.0f;
    // The camera-pitch band the widening lerps across, measured on |vpn.z|:
    // flat-to-the-horizon at or below LOW, fully back to base at or above HIGH.
    const float KSNAP_AXIS_FLAT_LOW    = 0.35f;
    const float KSNAP_AXIS_FLAT_HIGH   = 0.70f;
    // Half-length of the guide (and of the segment the pixel test runs against),
    // in PIXELS at the anchor's depth — so it reads the same at any zoom.
    const float KSNAP_AXIS_GUIDE_PIX   = 420.0f;
    // An axis whose projection is shorter than this is edge-on (a vertical seen
    // from straight above is a DOT): there is nothing to aim along, so refuse it.
    const float KSNAP_AXIS_MIN_PROJ    = 8.0f;
    // The dashed guide.  The pitch is a FRACTION OF THE GUIDE, not a pixel size:
    // the half-length is clamped in WORLD units at both ends (see ScanAxes), so a
    // pixel pitch would come adrift of it at extreme zooms and paint 130 tiny
    // dashes over the first 2% of the line.  32 dashes per side always spans it.
    const int   KSNAP_AXIS_DASHES      = 32;
    const float KSNAP_AXIS_DASH_DUTY   = 0.55f;   // ink : pitch
    const int   KSNAP_AXIS_MAX_SEGS    = 96;      // 2*32 dashes + the anchor dot
    // Dim grey, the same 0xaaaaaa family Plasticity's axis helper uses — but as a
    // COLOUR rather than an opacity, because kiwi_lines has no alpha (TRAP 2).
    const float KSNAP_COL_AXIS[3]      = { 0.42f, 0.42f, 0.46f };

    // Latched by the query for the guide pass and the label.
    bool        s_axisHave      = false;
    float       s_axisAnchor[3] = { 0.0f, 0.0f, 0.0f };
    float       s_axisDir[3]    = { 0.0f, 0.0f, 1.0f };
    float       s_axisHalfLen   = 0.0f;
    const char *s_axisName      = "Z";

    // v3: the angle SNAP_ANGLE resolved to, for the label ("15 deg").
    float s_angleDeg = 0.0f;
    // SHAKEOUT F: when the tool's anchor lies ON an existing line, the increments
    // are measured from THAT line instead of from the plane's u axis, and the
    // label says so ("45 deg rel").  s_angleRel is the angle relative to the base.
    bool  s_angleIsRel = false;
    float s_angleRel   = 0.0f;

    inline float Dot3( const float *a, const float *b )
    {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }

    // SHAKEOUT H FIX: the two the intersection arm's parallel/shared-endpoint
    // rejections need.  Same spelling as every other copy in this layer
    // (kiwi_construct.cpp, kiwi_transform.cpp) — three lines each, duplicated
    // rather than exported for the same reason WorldPerPixel below is.
    inline void Sub3( const float *a, const float *b, float *o )
    { o[0]=a[0]-b[0]; o[1]=a[1]-b[1]; o[2]=a[2]-b[2]; }
    inline void Cross3( const float *a, const float *b, float *o )
    {
        o[0] = a[1]*b[2] - a[2]*b[1];
        o[1] = a[2]*b[0] - a[0]*b[2];
        o[2] = a[0]*b[1] - a[1]*b[0];
    }

    // World units per screen pixel at the depth of `world` — the scale that makes
    // a marker screen-constant.
    // ROUND M: this WAS a private copy of the perspective per-pixel scale.  It is
    // now a thin alias over KiwiCam_WorldPerPixel (as kiwi_hover.cpp's has been
    // since shakeout A), because that body carries the ORTHOGRAPHIC arm — an ortho
    // view's scale is depth-INDEPENDENT, and a private copy would have gone on
    // shrinking the snap markers with distance in a projection that shrinks
    // nothing.  `c` is redundant (the shared body reads Ed_Camera()) but keeps
    // every call site below unchanged.
    inline float WorldPerPixel( const camera_s *c, const float *world )
    {
        (void)c;
        return KiwiCam_WorldPerPixel( world );
    }

    void Nudge( const camera_s *c, const float *in, float *out )
    {
        // Same -0.25 * vpn trick the hover pass documents: the marker sits ON
        // geometry that is already in the depth buffer, so nudge it toward the eye.
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

    // ── shakeout E: the Plasticity point glyph ──────────────────────────────
    // A camera-facing regular polygon of `segs` sides and world radius `r`,
    // optionally with its long diagonals filled in — which is how a 2 px hexagon
    // is made to read as a SOLID dot with a line renderer and no fill.
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
        // The three long diagonals of the hexagon: at this size they close the
        // interior visually without costing more than three segments.
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

    // Dot + separated ring.  The one glyph every POINT-rank snap now uses.
    void EmitDotAndRing( const camera_s *c, const float *p, float wpp )
    {
        EmitDisc( c, p, KSNAP_DOT_PIX  * wpp, KSNAP_DOT_SEGS,  true  );
        EmitDisc( c, p, KSNAP_RING_PIX * wpp, KSNAP_RING_SEGS, false );
    }

    // Ray ∩ the Z = 0 ground plane (§17's grid).  False when the ray is parallel
    // to it or points away from it.
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
        // KIWI-UX (ROUND X, ITEM 2): the ground plane is FINITE too — the same
        // Plasticity quad rule the working plane got (kiwi_construct.h
        // KiwiCon_RayPlaneBounded).  Without it a grazing ray lands the "grid"
        // snap tens of thousands of units past anything the grid actually draws.
        // Centred on the CAMERA's own ground projection rather than on the world
        // origin, because that is where kiwi_grid.cpp centres the lattice it draws
        // (`w.ground = c->origin[0..1]`, kiwi_grid.cpp:193-194) — so the bound and
        // the visible grid are the same window.
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

    // The two world endpoints of the edge a SEL_EDGE item names (kiwi_selection.h
    // DESIGN NOTE 3: w->p[e] → w->p[(e+1) % numpoints] of def->faces[f].w).
    bool EdgeEndpoints( const sel_item_t &it, float *a, float *b )
    {
        if ( !it.brush || !it.brush->def )
            return false;
        brush_t *def = it.brush->def;
        if ( !def->faces || it.faceIndex < 0 || it.faceIndex >= def->faceCount )
            return false;
        winding_t *w = def->faces[it.faceIndex].w;
        if ( !w || w->numpoints < 2 )
            return false;
        if ( it.edgeIndex < 0 || it.edgeIndex >= w->numpoints )
            return false;
        const int j = ( it.edgeIndex + 1 ) % w->numpoints;
        for ( int k = 0; k < 3; ++k )
        {
            a[k] = w->p[it.edgeIndex][k];
            b[k] = w->p[j][k];
        }
        return true;
    }

    // ═════════════════════════════════════════════════════════════════════════
    //  v3 — the CONSTRUCTION candidate scans (kiwi_construct.h scope ruling 1:
    //  construction geometry is not in the Pick candidate set, so it is walked
    //  here).  All three take the cursor pixel and return the best candidate.
    // ═════════════════════════════════════════════════════════════════════════
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

    // ── KIWI-UX (ROUND AP, ITEM 2): ONE GATE FOR EVERY CONSTRUCTION WALK ─────
    // Every arm below asked the SAME two-part question in five copied places, and
    // round AP had to add a third part to all of them, so it is one function now.
    //   * `hidden` (ROUND U): the user hid it; it is not snappable.
    //   * `KiwiConSel_SnapMuted` (ROUND AP, ITEM 2): a construction MOVE is
    //     dragging this object right now.  A transform must never snap the geometry
    //     it is dragging to itself — kiwi_transform.cpp says exactly that and hands
    //     down PICKF_EXCLUDE_SELECTED to enforce it, but that flag only reaches the
    //     arms that go through Pick(), and construction candidates deliberately do
    //     not (kiwi_construct.h scope ruling 1).  So this was the one self-snap the
    //     editor still had, and because the geometry-snap arm is ABSOLUTE
    //     (`total = snapPos - m_ref`) it compounded frame over frame: the reported
    //     "teleports back and forth from the pivot point to the arrow".  The full
    //     derivation is in kiwi_conselect.h over KiwiConSel_SnapMuted.
    //
    // …EXCEPT WHILE PLACING THE PIVOT, which is the SAME exception PickFlags makes
    // one file over ("`m_pivotPlacing ? PICKF_NONE : PICKF_EXCLUDE_SELECTED`",
    // kiwi_transform.cpp): the points a user reaches for first when placing a pivot
    // are corners of the very thing the gesture is about, and a pivot that could not
    // land on the lines being moved would be useless.  It is safe as well as
    // symmetric — a live placement consumes MouseMove in TrackPivot and never
    // reaches Recompute, so no geometry moves and there is no loop to close.
    //
    // `i` is the store index the caller is walking, which is what the mute keys on.
    inline bool ConCandidateUsable( const kconObject_t *o, int i )
    {
        if ( !o || o->hidden )
            return false;
        if ( KiwiXform_PivotPlacing() )
            return true;
        return !KiwiConSel_SnapMuted( i );
    }

    // Arm 1: the nearest construction ANCHOR within PICK_VERT_PIXELS.
    void ScanConAnchors( float curX, float curY, conBest_t *best )
    {
        const int count = KiwiCon_Count();
        for ( int i = 0; i < count; ++i )
        {
            const kconObject_t *o = KiwiCon_At( i );
            // ROUND U (hidden) + ROUND AP, ITEM 2 (moved by a live gesture).
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
                if ( d > PICK_VERT_PIXELS )
                    continue;
                if ( best->hit && d >= best->dist )
                    continue;
                best->hit  = true;
                best->dist = d;
                best->pos[0] = w[0]; best->pos[1] = w[1]; best->pos[2] = w[2];
            }
        }
    }

    // Arm 5's construction half: the closest point ON a construction segment
    // within PICK_EDGE_PIXELS.  The closest point is found in SCREEN space and
    // then applied to the WORLD segment by the same parameter — exact for a
    // segment, and it keeps this in one projection pass like the anchor scan.
    void ScanConSegments( float curX, float curY, conBest_t *best )
    {
        const int count = KiwiCon_Count();
        for ( int i = 0; i < count; ++i )
        {
            const kconObject_t *o = KiwiCon_At( i );
            // ROUND U (hidden) + ROUND AP, ITEM 2 (moved by a live gesture).
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
                const float ex = bx - ax, ey = by - ay;
                const float len2 = ex * ex + ey * ey;
                float t = 0.0f;
                if ( len2 > 1.0e-6f )
                {
                    t = ( ( curX - ax ) * ex + ( curY - ay ) * ey ) / len2;
                    if ( t < 0.0f ) t = 0.0f;
                    if ( t > 1.0f ) t = 1.0f;
                }
                const float d = PixelDist( ax + ex * t, ay + ey * t, curX, curY );
                // KIWI-UX (ROUND K): the construction clickbox is KCON_LINE_PIXELS (10),
                // not the brush-edge PICK_EDGE_PIXELS (6) — kiwi_construct.h.
                if ( d > KCON_LINE_PIXELS )
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

    // ═════════════════════════════════════════════════════════════════════════
    //  ROUND P, arm 4c — THE AXIS LINES THROUGH THE TOOL'S LAST PLACED POINT.
    //  The full argument, the Plasticity cites and every deviation are on
    //  KiwiSnap_DrawAxisGuides in kiwi_snap.h.
    // ═════════════════════════════════════════════════════════════════════════
    struct axisCand_t
    {
        float       dir[3];
        const char *name;
        bool        vertical;
    };

    // Name a direction the way the world axes are named, so the label says "Z"
    // when it IS Z whichever candidate produced it.  A plane axis that lies along
    // no world axis keeps the plane's own letter.
    const char *AxisName( const float *d, const char *fallback )
    {
        if ( fabsf( d[0] ) > 0.999f ) return "X";
        if ( fabsf( d[1] ) > 0.999f ) return "Y";
        if ( fabsf( d[2] ) > 0.999f ) return "Z";
        return fallback;
    }

    // The capture radius for ONE candidate, widened as the camera flattens toward
    // the horizon (the "camera angle should really help" half of round P's
    // directive, and ROUND X's shallow-angle one).
    //
    // KIWI-UX (ROUND X, ITEM 2): the widening now applies to EVERY candidate, not
    // only the vertical, and its flat end is Plasticity's own 30 px line threshold.
    // `a` is retained because the ramp is per-candidate by construction and a later
    // per-axis rule belongs here rather than at the call site.
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

    // Build the candidate set: world Z, then the working plane's U and V with the
    // world-aligned duplicates dropped.  Returns how many were written (<= 3).
    // ── ROUND Y, ITEM 4: THE PLANE NORMAL IS A CANDIDATE TOO ────────────────
    // The directive's other half — "Make sure the axis guides still offer the
    // plane's U/V plus the plane NORMAL so the user can leave the plane
    // deliberately."  Round P offered world Z plus the plane's U and V, which is
    // the full basis ONLY while the plane is XY.  On a vertical working plane —
    // which is exactly what round Y's axis-view rung now hands you from a
    // front/back/left/right camera — the normal is the ONE direction that leaves
    // the plane, and it was the one direction with no guide.
    //
    // Plasticity offers it two ways and both are the same idea: `NormalAxisSnap`
    // (plasticity/src/editor/snaps/AxisSnap.ts:129-133), handed out by
    // `FaceSnap.additionalSnapsFor` (Snaps.ts:353-358) and selectable with `n`
    // (default-keymap.ts:354-360); and, in ortho mode, the whole world triple
    // ROTATED INTO THE PLANE'S BASIS —
    // `const quat = viewportInfo.isOrthoMode ? viewportInfo.constructionPlane
    // .orientation : new THREE.Quaternion(); … this.addAxesAt(snap.position, quat,
    // XYZ, …)` (PointPickerModel.ts:274-292), i.e. U, V and N.  KIWI gets there by
    // adding N to the U/V pair it already had.
    //
    // The dedup is unchanged and now covers all three: a candidate that IS the
    // world vertical is dropped, because world Z is always offered first and two
    // guides on one line is two answers to one aim.
    enum { KSNAP_AXIS_CANDS = 4 };           // world Z + plane U + plane V + plane N

    int GatherAxisCandidates( const kconPlane_t &plane, axisCand_t *out )
    {
        int n = 0;
        out[n].dir[0] = 0.0f; out[n].dir[1] = 0.0f; out[n].dir[2] = 1.0f;
        out[n].name = "Z";
        out[n].vertical = true;
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
                continue;                    // it IS the vertical — Plasticity's dedup
            out[n].dir[0] = d[0]; out[n].dir[1] = d[1]; out[n].dir[2] = d[2];
            out[n].name = AxisName( d, pn[i] );
            out[n].vertical = false;
            ++n;
        }
        return n;
    }

    // The scan.  Screen-space closest point on the axis SEGMENT, then the same
    // parameter applied to the world segment — the exact idiom ScanConSegments
    // uses, so the axis and a construction line are measured the same way and
    // their pixel distances are directly comparable.
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
                   float curX, float curY, axisBest_t *best )
    {
        const float wpp = WorldPerPixel( c, anchor );
        if ( !( wpp > 0.0f ) )
            return;
        float half = KSNAP_AXIS_GUIDE_PIX * wpp;
        if ( half < 32.0f )    half = 32.0f;
        if ( half > 32768.0f ) half = 32768.0f;

        axisCand_t cand[KSNAP_AXIS_CANDS];       // ROUND Y: was 3 — the plane NORMAL joined
        const int n = GatherAxisCandidates( plane, cand );

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

            float t = ( ( curX - ax ) * ex + ( curY - ay ) * ey ) / len2;
            if ( t < 0.0f ) t = 0.0f;
            if ( t > 1.0f ) t = 1.0f;
            const float d = PixelDist( ax + ex * t, ay + ey * t, curX, curY );
            if ( d > AxisRadius( c, cand[i] ) )
                continue;
            if ( best->hit && d >= best->dist )
                continue;

            // The world point, grid-snapped along the axis so "straight up,
            // exactly three cells" is one gesture.
            //
            // ABSOLUTE when the axis IS a world axis — the same rule ROUND P gave
            // the transform's grid arm and the same one the shakeout-H Z lock has
            // always used (it snaps the world z, kiwi_construct.cpp ApplyZLock).
            // An anchor that is itself off-grid (a brush corner, say) therefore
            // does not drag its offset up the axis with it.  On a SLANTED plane
            // axis there is no world coordinate to be on the grid of, so that case
            // quantises the DISTANCE from the anchor — narrowly and deliberately,
            // exactly as the push/pull scalar does.
            float       along = -use + ( 2.0f * use ) * t;
            const float g     = KiwiUnits_GridSpacingWorld();
            if ( g > 0.0f )
            {
                int wax = -1;
                for ( int k = 0; k < 3; ++k )
                    if ( fabsf( cand[i].dir[k] ) > 0.999f )
                        wax = k;
                if ( wax >= 0 )
                {
                    const float posw = anchor[wax] + cand[i].dir[wax] * along;
                    const float snap = floorf( posw / g + 0.5f ) * g;
                    along = ( snap - anchor[wax] ) / cand[i].dir[wax];
                }
                else
                {
                    along = floorf( along / g + 0.5f ) * g;
                }
            }

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

    // ── SHAKEOUT F, arm 3b: construction segment MIDPOINTS ──────────────────
    // USER DIRECTIVE: "We also need snap points in the middle-section of each
    // edge/line."  Brush edges already had one (arm 4); construction segments did
    // not, which is the half the user was actually drawing against.  Same rank and
    // same type as arm 4 (SNAP_EDGE_MID) — a midpoint is a midpoint — and the same
    // POINT radius, so it outranks the LINE snap that owns the rest of the segment
    // only in the few pixels where the cursor is genuinely on the middle.
    void ScanConMidpoints( float curX, float curY, conBest_t *best )
    {
        const int count = KiwiCon_Count();
        for ( int i = 0; i < count; ++i )
        {
            const kconObject_t *o = KiwiCon_At( i );
            // ROUND U (hidden) + ROUND AP, ITEM 2 (moved by a live gesture).
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
                if ( d > PICK_VERT_PIXELS )
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

    // Arm 3: where two construction segments cross.
    //
    // ── SHAKEOUT H: SOLVED IN 3D, NOT ON THE ACTIVE PLANE ───────────────────
    // v3 gathered only the segments lying on the ACTIVE construction plane and
    // solved a 2D line-line intersection there.  With a world-space store
    // (kiwi_construct.h ruling 3) that gate throws away almost everything: two
    // lines drawn at different heights, or on a wall while the active plane is the
    // floor, are exactly the crossings the user wants to snap to and the old test
    // never even looked at them.
    //
    // Now: pairwise CLOSEST APPROACH in 3D (KiwiCon_SegSegClosest — the same helper
    // the trim tool uses, so what snaps is what trims), accepted when the two
    // segments come within KCON_ISECT_DIST (0.25 units) of each other, and the
    // reported point is the midpoint of the two closest points.  An exact 3D
    // intersection would be an exact-zero test on floats and would never fire.
    //
    // ── SHAKEOUT H FIX: THREE REJECTIONS THE 3D REWRITE DROPPED ─────────────
    // The 2D solve it replaced rejected `fabsf(denom) < 1e-6` — parallel and
    // collinear pairs — and it could not see same-object pairs at all, because it
    // only admitted segments lying on the active plane and solved them as infinite
    // lines.  Closest-approach has neither property, and the omissions were not
    // subtle:
    //
    //   1. ADJACENT SEGMENTS OF THE SAME OBJECT share a vertex, so their closest
    //      approach is exactly ZERO.  Every tessellation vertex of every circle,
    //      arc, polyline and rect in the store became a fake SNAP_INTERSECTION —
    //      64 of them per circle — burying the real crossings, out-ranking
    //      SNAP_EDGE_MID and SNAP_EDGE below it, and eating the pair budget.
    //      Same-object pairs are still ALLOWED when they are non-adjacent: a
    //      polyline that genuinely crosses itself has a genuine intersection there.
    //   2. COLLINEAR OVERLAPPING pairs (two lines drawn along the same edge, which
    //      is exactly what scaffolding looks like) have zero gap along their whole
    //      overlap, and the "intersection" is then wherever the clamp happened to
    //      land.  Rejected on the sine of the angle between them.
    //   3. TWO OBJECTS MEETING END TO END — the commonest thing in this store,
    //      since that is what a chain IS — report their shared endpoint as an
    //      intersection.  That point is already SNAP_ENDPOINT (arm 1, which
    //      out-ranks this one), so the duplicate only costs budget and can win
    //      when the anchor scan is off by a pixel.  Rejected explicitly.
    void ScanConIntersections( float curX, float curY, conBest_t *best )
    {
        // Gather the world segments once, bounded exactly as before — plus the
        // OWNER and ORDINAL each one came from, which is what makes rejection 1
        // expressible at all.
        struct seg3_t { float a[3], b[3]; int obj, ord; bool wrapEnd; };
        seg3_t segs[KSNAP_MAX_CSEGS];
        int    segCount = 0;

        const int count = KiwiCon_Count();
        for ( int i = 0; i < count && segCount < KSNAP_MAX_CSEGS; ++i )
        {
            const kconObject_t *o = KiwiCon_At( i );
            // ROUND U (hidden) + ROUND AP, ITEM 2 (moved by a live gesture).
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
                // ── rejection 1: adjacent segments of ONE object ─────────────
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
                // ── rejection 2: parallel / collinear.  |d1 x d2| / (|d1||d2|) is
                // the sine of the angle between them; below this they are the same
                // line and "where they cross" has no answer.  1e-3 rad is about
                // 0.06 degrees — tight enough that a real 0.5-degree crossing still
                // resolves, loose enough that two hand-drawn parallels never do.
                if ( sqrtf( Dot3( cr, cr ) ) / ( l1 * l2 ) < 1.0e-3f )
                    continue;

                float w[3], px, py;
                const float gap = KiwiCon_SegSegClosest( segs[i].a, segs[i].b,
                                                         segs[j].a, segs[j].b,
                                                         0, 0, w );
                if ( gap > KCON_ISECT_DIST )
                    continue;                 // they do not meet

                // ── rejection 3: this is a SHARED ENDPOINT, not a crossing ───
                // Both tests are needed: the ends have to actually coincide AND the
                // crossing has to be AT them.  Two lines that meet at a T-junction
                // in the middle of one of them share no endpoint and survive.
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
                if ( d > PICK_VERT_PIXELS )
                    continue;
                if ( best->hit && d >= best->dist )
                    continue;
                best->hit  = true;
                best->dist = d;
                best->pos[0] = w[0]; best->pos[1] = w[1]; best->pos[2] = w[2];
            }
        }
    }

    // ═════════════════════════════════════════════════════════════════════════
    //  SHAKEOUT F additions
    // ═════════════════════════════════════════════════════════════════════════

    // ── SNAP_FACE_CENTER: the winding CENTROID of the face under the cursor ──
    // USER DIRECTIVE: "Also in the center of each face."  It is a POINT-rank snap
    // (a centroid is a point), so it is tested with the POINT radius and ranked
    // with the other point arms — which also means it can only ever fire in the 8
    // pixels around the centroid itself and can never shadow the face's own edges
    // anywhere else on it.
    //
    // The face comes from ONE extra Pick with SEL_MASK_FACE.  That mask (and NOT
    // the object mask arm 6 already runs) is what makes Test_Ray's hit resolve to
    // a `faceIndex` at all — kiwi_pick.cpp's `faceGranularity` requires the FACE
    // bit set and the OBJECT bit clear.  Cost is one more candidate walk per
    // query; the header's budget note records the rise from three to four.
    //
    // v1 IS BRUSH FACES ONLY.  A patch has no winding and its "centre" would be a
    // tessellation question, not a geometry one; logged in RADIANT_KNOWN_ISSUES
    // rather than guessed at.
    bool FaceCentroid( const sel_item_t &it, float *out )
    {
        if ( !it.brush || !it.brush->def )
            return false;
        // def->faces (face_t, GEOMETRY), never node->faces (faceVis_s, VISIBILITY)
        // — the same distinction EdgeEndpoints above depends on.
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

    // ── RELATIVE ANGLE SNAP: what line is the tool's ANCHOR sitting on? ──────
    // USER DIRECTIVE: "When drawing a line on top of an existing line/edge, it
    // also will do a nice angle snap when applicable as well."
    //
    // The anchor-on-line test is literal: point-to-segment distance under the
    // store's weld tolerance (KREG_JOIN_DIST — the SAME number that decides two
    // endpoints are the same endpoint, so "on it" means one thing everywhere).
    // The winner is the CLOSEST such segment, construction and brush edges
    // competing on equal terms, because a user drawing off a corner where both
    // meet means the one they aimed at and the pixels cannot tell us which.
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

    // Brush edges the relative-angle arm may examine in one query.  The bbox
    // reject below throws out whole brushes for the cost of six compares, so this
    // ceiling is a backstop against a pathological map, not a normal-case budget.
    enum { KSNAP_MAX_ANCHOR_EDGES = 4096 };

    bool AnchorLineDir( const float anchor[3], float outDir[3] )
    {
        // 0. THE IN-PROGRESS CHAIN WINS OUTRIGHT.  The segment the user just
        //    placed with this very tool is not in the store yet (a drawing tool
        //    commits its object at Finish, not per click), so without this rung a
        //    polyline could never turn 45° off its own previous segment — the
        //    single most obvious case the directive describes.  It is also the
        //    least ambiguous: it is the thing the user drew a moment ago.
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
                        outDir[k] = dir[k] / l;
                    return true;
                }
            }
        }

        const float tol  = KREG_JOIN_DIST;
        float       best = tol * tol;
        bool        have = false;

        // 1. construction segments
        const int count = KiwiCon_Count();
        for ( int i = 0; i < count; ++i )
        {
            const kconObject_t *o = KiwiCon_At( i );
            // ROUND U (hidden) + ROUND AP, ITEM 2 (moved by a live gesture).
            if ( !ConCandidateUsable( o, i ) )
                continue;
            const int segs = KiwiCon_SegmentCount( *o );
            for ( int s = 0; s < segs; ++s )
            {
                float wa[3], wb[3];
                if ( !KiwiCon_SegmentWorld( *o, s, wa, wb ) )
                    break;
                const float d2 = PointSegDist2( anchor, wa, wb );
                if ( d2 > best )
                    continue;
                float dir[3];
                for ( int k = 0; k < 3; ++k )
                    dir[k] = wb[k] - wa[k];
                const float l = sqrtf( Dot3( dir, dir ) );
                if ( !( l > 1.0e-4f ) )
                    continue;
                best = d2;
                have = true;
                for ( int k = 0; k < 3; ++k )
                    outDir[k] = dir[k] / l;
            }
        }

        // 2. brush edges.  Whole-brush bbox reject first (expanded by the
        //    tolerance), so a map's worth of brushes costs six compares each.
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
                        if ( d2 > best )
                            continue;
                        float dir[3];
                        for ( int k = 0; k < 3; ++k )
                            dir[k] = w->p[j][k] - w->p[i][k];
                        const float l = sqrtf( Dot3( dir, dir ) );
                        if ( !( l > 1.0e-4f ) )
                            continue;
                        best = d2;
                        have = true;
                        for ( int k = 0; k < 3; ++k )
                            outDir[k] = dir[k] / l;
                    }
                }
            }
        }
        return have;
    }
}

// ─── settings ────────────────────────────────────────────────────────────────
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
    case SNAP_ENDPOINT:     return "end";       // v3: a CONSTRUCTION anchor
    case SNAP_INTERSECTION: return "isect";
    case SNAP_AXIS:         return "axis";
    case SNAP_CPLANE:       return "cplane";
    case SNAP_ANGLE:        return "angle";
    default:                return "off";
    }
}

// ─── ROUND Z, ITEM 3: a SURFACE snap resolved on a ONE-AXIS gesture ──────────
// USER REPORT, verbatim: "When snapping an extrusion to another face.  The entire
// face should give the same result.  Right now it's going up or down slightly more
// or less based on where the face is - which makes no sense for a flat 'roof' on a
// brush.  It does this in side view (cube), but it doesn't make sense for it to not
// be the case at all angles for a flat horizontal surface."
//
// WHAT WAS WRONG.  All three one-axis gestures resolved a geometry snap the same
// way — `dot( snapPos - ref, axis )` — and for arm 6 (SNAP_FACE) `snapPos` is the
// RAY-SURFACE HIT: a point that slides across the target face as the cursor moves.
// Projecting a sliding point onto the axis gives a sliding answer, and the amount
// it slides is exactly the component of the face's extent that lies ALONG the push
// direction.  A wall (normal perpendicular to the push) is the pathological case:
// the projection is then pure noise — the height the cursor happens to be at.
//
// THE RULE.  A face is not a point, it is a PLANE, and "extrude up to that face"
// has one answer: where the plane crosses the gesture's own axis.
//     t = ( n · ( hit - ref ) ) / ( n · axis )
// One scalar for the whole face, independent of where on it the cursor is, and
// identical to the old projection in the case that was already right (n parallel to
// axis, where the denominator is ±1).  The hit point is used as the plane's
// reference point rather than face.plane.dist because the hit is BY CONSTRUCTION on
// the plane the arm reported, so this cannot disagree with what the user is
// pointing at even if the cached plane is stale.
//
// A NEAR-PARALLEL FACE HAS NO ANSWER and is REFUSED (false), which is the fix's
// other half: a vertical wall can no longer teleport a vertical push to wherever
// the cursor grazed it.  KSNAP_AXIS_PARALLEL is the cut-off.
//
// POINT candidates — vertices, edge points, edge and face MIDPOINTS, construction
// anchors and intersections — are projected exactly as before.  Those name one
// position and projecting a position is the correct question to ask of it; only the
// AREA arm was ever answering a different one.
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
        const sel_item_t &it = r.source;
        // Sel_BrushLive first: the callers hold the snap result across frames (a
        // typed value re-runs Recompute with last frame's snap), so this pointer is
        // a CACHED one by the time it is read — kiwi_selection.h LIVENESS.
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
        // No usable plane (a patch, a dead node, a degenerate normal): fall through
        // to the point projection rather than dropping the snap entirely.
    }

    *outDist = Dot3( rel, axis );
    return true;
}

// ─── ROUND AG, ITEM 11: the light grid magnet ────────────────────────────────
// See kiwi_snap.h LIGHT GRID SNAPPING for the directive, for why this is not a
// walk-back of D-Z2, and for the "band, not quantiser" argument.
float KiwiSnap_LightGridAxis( float d, const float *ref, const float *axis )
{
    // ROUND AJ, ITEM 5: the grid-snap master switch (kiwi_grid.h) covers the BAND
    // as well as the quantiser — a "disable snapping to grid" that left a magnet
    // pulling the value around would not be the checkbox the user asked for.  The
    // refusal is the same "leave `d` alone" the degenerate-spacing arm already is.
    if ( !KiwiGrid_SnapEnabled() )
        return d;
    const float g = KiwiUnits_GridSpacingWorld();
    if ( !( g > 0.0f ) || !ref || !axis )
        return d;

    // Round P's rule: an axis-aligned gesture puts the ABSOLUTE coordinate on the
    // lattice; a slanted one has no world coordinate to be on the grid of and
    // snaps its own distance.  `target` is the value we want on the lattice and
    // `scale` converts a change in it back into a change in `d`.
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

    const float snapped = floorf( value / g + 0.5f ) * g;

    // The band, in screen pixels at the gesture's CURRENT point, capped so that
    // zooming out cannot turn the magnet into a quantiser.
    float at[3];
    for ( int k = 0; k < 3; ++k )
        at[k] = ref[k] + axis[k] * d;
    float band = KSNAP_LIGHT_BAND_PIX * KiwiCam_WorldPerPixel( at );
    const float maxBand = g * KSNAP_LIGHT_MAX_FRAC;
    if ( !( band > 0.0f ) )  band = maxBand;          // NaN / degenerate camera
    if ( band > maxBand )    band = maxBand;

    if ( fabsf( value - snapped ) > band )
        return d;                                     // outside the band: untouched
    return d + ( snapped - value ) * scale;
}

// ─── the query ───────────────────────────────────────────────────────────────
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
    s_axisHave    = false;                   // ROUND P — re-latched by arm 4c

    // ── the cursor pixel, from the RAY (exact: it round-trips CameraCalcRayDir,
    //    same trick kiwi_pick.cpp's screen pass uses) ──────────────────────────
    const float ahead[3] = { ray.origin[0] + ray.dir[0],
                             ray.origin[1] + ray.dir[1],
                             ray.origin[2] + ray.dir[2] };
    float curX = 0.0f, curY = 0.0f;
    const bool haveCursorPx = Pick_WorldToImage( ahead, &curX, &curY );

    // ── v3: the active construction plane, when a drawing tool owns the gesture ─
    // KIWI-UX (ROUND K): PlanePlacement, not ToolActive — the §16b primitives place
    // on the working plane exactly as the drawing tools do, and gating arms 6/7/8
    // on "is a KiwiDrawTool running" is the whole box-creation bug
    // (kiwi_construct.h KiwiCon_SetPlanePlacement).  With no primitive live the two
    // predicates are identical, so every other answer is unchanged.
    const bool toolActive = KiwiCon_PlanePlacement();
    const kconPlane_t &cplane = KiwiCon_ActivePlane();
    float cplaneHit[3];
    // KIWI-UX (ROUND X, ITEM 2): the BOUNDED form.  The infinite one resolved a
    // grazing ray a hundred thousand units out (so the placed point left the drawn
    // grid) and failed outright when the plane was a hair behind the eye (so arms
    // 7+8 dropped and a drawing tool placed OFF its own plane).  Both are the
    // directive's "impossible to snap/guide the line onto the xy plane".  The
    // bounded form is Plasticity's finite quad — see kiwi_construct.h.
    const bool haveCPlane = toolActive && KiwiCon_RayPlaneBounded( cplane, ray, cplaneHit );

    // ── the surface hit (arm 6) + the RAW point arms 0 and 9 fall back to ─────
    const pick_result_t surf = Pick( ray, SEL_MASK_OBJECT, pickFlags );
    float raw[3];
    if ( haveCPlane )
    {
        // A drawing tool places on ITS OWN plane — even with snapping suppressed,
        // and even when the ray happens to hit a wall behind it.
        raw[0] = cplaneHit[0];
        raw[1] = cplaneHit[1];
        raw[2] = cplaneHit[2];
    }
    else if ( surf.valid )
    {
        raw[0] = surf.point[0];
        raw[1] = surf.point[1];
        raw[2] = surf.point[2];
    }
    else if ( !RayHitsGroundPlane( ray, raw ) )
    {
        RayPoint( ray, KSNAP_FALLBACK_DIST, raw );
    }

    // ── arm 0: CTRL suppresses snapping (§6) ─────────────────────────────────
    // GetAsyncKeyState is the physical-state read every ported drag path uses
    // (drag.cpp:308 etc.); this file runs post-present from the pump, same as those.
    //
    // ── KIWI-UX (ROUND Z, ITEM 2): …AND FOR ONE-AXIS GESTURES IT ENABLES IT ──
    // USER DIRECTIVE, verbatim: "When extruding, it should not snap by default.
    // Make it snap only when holding CTRL.  It's just not good to use in a
    // cluttered scene."
    //
    // The test is an XOR against the ACTIVE COMMAND'S DEFAULT rather than a second
    // arm, which is exactly Plasticity's shape:
    // `get enabled() { return this._enabled !== this.xor }` (SnapManager.ts:28-49),
    // with Ctrl driving `xor` on keydown/keyup (default-keymap.ts:353,365-366).
    // Ctrl still means "the other one"; only what "the one" is has changed, and only
    // for the commands that ask (kiwi_command.h SnapOptIn — the face push/pull, the
    // region extrude and the face extrude / un-extrude, and nothing else).
    //
    // SUPPRESSED IS AN ANSWER, NOT A FAILURE (the header's arm 9 note): `valid` stays
    // true and `position` is the raw point, so every consumer that tests
    // `type != SNAP_NONE` — which is all three one-axis gestures — falls back to its
    // own raw cursor mapping, ungridded, with the numeric field untouched.
    const bool ctrlHeld = ( ::GetAsyncKeyState( VK_CONTROL ) & 0x8000 ) != 0;
    if ( ctrlHeld != KiwiCmd_SnapOptIn() )
    {
        out->valid = true;
        out->type  = SNAP_NONE;
        out->position[0] = raw[0];
        out->position[1] = raw[1];
        out->position[2] = raw[2];
        return true;
    }

    // ── the construction candidates, gathered once (kiwi_snap.h budget note) ──
    conBest_t conAnchor, conSeg, conIsect, conMid;
    if ( haveCursorPx && KiwiCon_Count() > 0 )
    {
        ScanConAnchors      ( curX, curY, &conAnchor );
        ScanConIntersections( curX, curY, &conIsect );
        ScanConMidpoints    ( curX, curY, &conMid );      // shakeout F
        ScanConSegments     ( curX, curY, &conSeg );
    }

    // ═════════════════════════════════════════════════════════════════════════
    //  ARM 0b (ROUND AA, ITEM 3a) — THE OPEN CHAIN'S FIRST POINT, TOP PRIORITY
    // ═════════════════════════════════════════════════════════════════════════
    // USER REPORT, verbatim: "We need a light snapping point here for this line
    // connect."
    //
    // The one target a closing gesture is aiming at was the one point in the scene
    // this query could not see.  The in-progress chain is not in the construction
    // store yet (the tool holds it in m_pts and only commits on Finish), so arm 1
    // scans right past it.  KiwiCon_ToolLoopStart (kiwi_construct.h) exposes it,
    // and answers only while a chain of >= 3 points is open.
    //
    // ── PLASTICITY DOES THIS, AND THE THRESHOLD IS THEIRS ───────────────────
    // `addSnaps` re-registers the curve's points on EVERY iteration of the point
    // loop (CurveCommand.ts:73-81), and the start point is the one that gets a
    // display name:
    //
    //     if ( makeCurve.canBeClosed ) {
    //         for ( const point of makeCurve.otherPoints )
    //             pointPicker.addSnap( new PointSnap( undefined, point ) );
    //         pointPicker.addSnap( new PointSnap( "Closed", makeCurve.startPoint ) );
    //     }
    //
    // gated on `get canBeClosed() { return this.underlying.points.length >= 3; }`
    // (CurveFactory.ts:185-187) — which is where the >= 3 in KiwiCon_ToolLoopStart
    // comes from, rather than from a guess.
    //
    // ── AND HERE IS THE DELIBERATE DEVIATION, STATED ────────────────────────
    // Their "Closed" snap is a plain PointSnap, so its priority is 1 — the SAME as
    // every vertex and endpoint in the scene (SnapPicker.ts:136-161; only
    // FaceCenterPointSnap and TanTanSnap outrank a point, at 0.99).  Ties fall to
    // the pre-sort array order, so a model vertex under the cursor can beat their
    // closing point.  KIWI puts it ABOVE EVERY OTHER ARM instead, for two reasons
    // this layer has and Plasticity does not:
    //
    //   1. THEY DO NOT NEED THE SNAP TO WIN.  Their close test is a geometric
    //      coincidence check on whatever point came back —
    //      `wouldBeClosed(p) { return points.length >= 2 &&
    //       p.manhattanDistanceTo(this.startPoint) < 10e-6 }` (CurveFactory.ts:114-116)
    //      — so ANY snap that lands exactly on the start point closes the curve.
    //      KIWI's close test is a PIXEL box on the cursor (NearFirstPoint /
    //      KCON_JOIN_PIXELS), which closes the loop but does not move the point:
    //      a competing vertex 3 px away would close the ring visually and store a
    //      point that is not the first one.
    //   2. WHAT THAT COSTS IS THE WHOLE SHAPE.  A ring with a hairline gap is not
    //      a region (kiwi_region.cpp's ring test), and the gap is far too small to
    //      see, so the user gets "it just didn't work" with nothing to look at.
    //      That is the reported bug one room over (ITEM 3b), and it is not worth
    //      re-introducing through the back door for the sake of a tie rule.
    //
    // Reported as SNAP_ENDPOINT — it IS a construction endpoint, it is drawn by
    // the accent language that already exists for one, and it counts as a geometry
    // snap (KiwiSnap_IsGeometry) so every consumer treats it as the exact position
    // it is.  The radius is PICK_VERT_PIXELS, the same 8 px every other point
    // candidate uses; the CLICK that closes the loop keeps its own wider
    // KCON_JOIN_PIXELS box, so the snap engages inside the box that closes rather
    // than the other way round.
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

    // ── arm 1: POINT — a CONSTRUCTION anchor (8 px) ──────────────────────────
    if ( conAnchor.hit )
    {
        out->valid = true;
        out->type  = SNAP_ENDPOINT;
        out->position[0] = conAnchor.pos[0];
        out->position[1] = conAnchor.pos[1];
        out->position[2] = conAnchor.pos[2];
        return true;                     // `source` stays the null item — see the header
    }

    // ── arm 2: POINT — a winding corner / patch control point (8 px) ─────────
    const pick_result_t vert = Pick( ray, SEL_MASK_VERTEX, pickFlags );
    if ( vert.valid )
    {
        out->valid  = true;
        out->type   = SNAP_VERTEX;
        out->source = vert.item;
        out->position[0] = vert.point[0];
        out->position[1] = vert.point[1];
        out->position[2] = vert.point[2];
        return true;
    }

    // ── arm 3: POINT — a construction segment crossing (8 px) ────────────────
    if ( conIsect.hit )
    {
        out->valid = true;
        out->type  = SNAP_INTERSECTION;
        out->position[0] = conIsect.pos[0];
        out->position[1] = conIsect.pos[1];
        out->position[2] = conIsect.pos[2];
        return true;
    }

    // ── arm 3b (shakeout F): POINT — a CONSTRUCTION segment MIDPOINT (8 px) ──
    // Above the brush midpoint for the same reason arm 1 sits above arm 2:
    // construction geometry is scaffolding the user placed on purpose and is
    // aiming at on purpose.
    if ( conMid.hit )
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

    // ── arms 4 + 5: the nearest brush edge, resolved to its MIDPOINT or to the
    //    point on it — ONE pick, two ranks (see the header's ranking note).
    const pick_result_t edge = Pick( ray, SEL_MASK_EDGE, pickFlags );
    if ( edge.valid )
    {
        float ea[3], eb[3];
        const bool haveEnds = EdgeEndpoints( edge.item, ea, eb );
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

            // arm 4: the midpoint is a POINT snap — test it with the POINT radius.
            const float mid[3] = { ( ea[0] + eb[0] ) * 0.5f,
                                   ( ea[1] + eb[1] ) * 0.5f,
                                   ( ea[2] + eb[2] ) * 0.5f };
            float mx, my;
            if ( haveCursorPx && Pick_WorldToImage( mid, &mx, &my )
              && PixelDist( mx, my, curX, curY ) <= PICK_VERT_PIXELS )
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

    // ── arm 4b (shakeout F): POINT — the FACE CENTRE under the cursor (8 px) ──
    // Last of the point ranks: a centroid is a derived point, so anything the user
    // actually placed or built outranks it.
    //
    // SHAKEOUT H: NO LONGER SUPPRESSED WHILE A DRAWING TOOL RUNS.  The suppression
    // existed because "a tool places on its OWN plane and a face centre would
    // quietly drag the point off it" — which was true of ruling 3 as it stood and
    // is now the OPPOSITE of what is wanted: a drawing tool places AT the geometry
    // snap (kiwi_construct.cpp MouseMove), so a face centre is a perfectly good
    // place to start a line and refusing to offer it was one of the reasons lines
    // could not leave the plane.  Arm 6 (the AREA face snap) stays suppressed: it
    // fires on any pixel of any surface, which would turn every drag over a wall
    // into a surface crawl instead of a plane placement.
    if ( haveCursorPx )
    {
        const pick_result_t faceHit = Pick( ray, SEL_MASK_FACE, pickFlags );
        float centre[3];
        float cx, cy;
        if ( faceHit.valid && FaceCentroid( faceHit.item, centre )
          && Pick_WorldToImage( centre, &cx, &cy )
          && PixelDist( cx, cy, curX, curY ) <= PICK_VERT_PIXELS )
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

    // ── ROUND P, arm 4c: the AXIS LINES through the tool's last placed point ──
    // Gathered here, immediately above the LINE rank it belongs to, and resolved
    // as part of arm 5's "closest pixel wins" below — see kiwi_snap.h for why it
    // competes rather than taking a fixed priority the way Plasticity's does.
    axisBest_t axisBest;
    if ( haveCursorPx && toolActive )
    {
        float anchor[3];
        if ( KiwiCon_ToolAnchor( anchor ) )
            ScanAxes( Ed_Camera(), anchor, cplane, curX, curY, &axisBest );
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

    // ── arm 5: LINE — brush edge vs construction segment vs AXIS, closest wins ─
    if ( edge.valid || conSeg.hit || axisBest.hit )
    {
        const bool useCon = conSeg.hit && ( !edge.valid || conSeg.dist < edge.screenDist );
        const float lineDist = useCon ? conSeg.dist
                                      : ( edge.valid ? edge.screenDist : 1.0e9f );
        if ( axisBest.hit && axisBest.dist <= lineDist )
        {
            out->valid = true;
            out->type  = SNAP_AXIS;
            out->position[0] = axisBest.pos[0];
            out->position[1] = axisBest.pos[1];
            out->position[2] = axisBest.pos[2];
            // The marker's tick runs ALONG the axis, exactly as it does along an
            // edge — the axis IS the line this snap is on.
            s_haveEdgeDir = true;
            for ( int k = 0; k < 3; ++k )
                s_edgeDir[k] = axisBest.dir[k];
            return true;                     // `source` stays the null item
        }
        // An axis that came within its radius but LOST to a nearer real line keeps
        // its guide drawn (s_axisHave stands): the guide is "you are near the
        // vertical", which is true whether or not it is what the click will take.
        // That is Plasticity's rule too — every hit snap contributes its helper
        // (SnapPresenter.ts:71-84), not just the winner.
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

    // ── arm 6: AREA — the Test_Ray surface point, UNSNAPPED (see the header) ──
    // Suppressed while a drawing tool runs: the tool places on its own plane, and
    // an area snap would quietly move the point off it.
    if ( surf.valid && !toolActive )
    {
        out->valid  = true;
        out->type   = SNAP_FACE;
        out->source = surf.item;
        out->position[0] = raw[0];
        out->position[1] = raw[1];
        out->position[2] = raw[2];
        return true;
    }

    // ── arms 7 + 8: the construction plane, with the 15° angle lock on top ────
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
                // arm 7: quantise the DIRECTION and keep the distance, so the
                // segment stays exactly as long as the cursor says it is.
                //
                // ── SHAKEOUT F: the increments have a BASE DIRECTION now ─────
                // USER DIRECTIVE: "When drawing a line on top of an existing
                // line/edge, it also will do a nice angle snap when applicable as
                // well."  v3 measured its 15° stops from the plane's u axis and
                // nothing else, which is right over empty space and wrong the
                // moment the user starts a segment ON something: continuing a
                // slanted wall's edge at "square to it" was unreachable unless the
                // wall happened to be square to the plane basis.
                //
                // So the base is now the direction of whatever LINE the ANCHOR is
                // sitting on (AnchorLineDir — construction segment or brush edge,
                // whichever is nearer, within the store's weld tolerance), and 0°
                // means "along that line".  The INCREMENT is unchanged
                // (KCON_ANGLE_STEP, one rule in one place), so the stops include
                // 0/45/90/135 relative exactly as the directive describes and also
                // keep the 15° ladder the rest of the editor teaches.  With
                // nothing under the anchor the base is 0 and the behaviour is
                // BIT-IDENTICAL to v3.
                float baseDeg = 0.0f;
                {
                    float refDir[3];
                    if ( AnchorLineDir( anchor, refDir ) )
                    {
                        // Into the PLANE's basis — the same (u,v) the stops are
                        // measured in.  A reference line that is perpendicular to
                        // the plane has no in-plane bearing and is ignored.
                        const float ru = Dot3( refDir, cplane.u );
                        const float rv = Dot3( refDir, cplane.v );
                        if ( sqrtf( ru * ru + rv * rv ) > 1.0e-3f )
                        {
                            baseDeg      = atan2f( rv, ru ) * 57.29577951f;
                            s_angleIsRel = true;
                        }
                    }
                }

                const float rawDeg = atan2f( dv, du ) * 57.29577951f;
                float rel = rawDeg - baseDeg;
                rel = floorf( rel / KCON_ANGLE_STEP + 0.5f ) * KCON_ANGLE_STEP;
                const float deg = baseDeg + rel;
                const float rad = deg * 0.01745329252f;
                uv[0] = auv[0] + cosf( rad ) * len;
                uv[1] = auv[1] + sinf( rad ) * len;
                s_angleDeg = deg;
                // Reported in (-180, 180] so "90 deg rel" never prints as 450.
                while ( rel >  180.0f ) rel -= 360.0f;
                while ( rel <= -180.0f ) rel += 360.0f;
                s_angleRel = rel;
                KiwiCon_PlaneToWorld( cplane, uv, out->position );
                out->valid = true;
                out->type  = SNAP_ANGLE;
                return true;
            }
        }

        // arm 8: plain construction-plane placement, grid-snapped IN PLANE.
        // KIWI-UX (ROUND R): on the WORLD-ANCHORED lattice.  This is the arm the
        // user's screenshot was reading from — an active plane seated at an
        // arbitrary face-hit point made every cplane answer congruent to that
        // point's fractional part.  The whole chain is on KiwiCon_SnapUV
        // (kiwi_construct.h); the change here is that the plane goes in with the uv.
        KiwiCon_SnapUV( cplane, uv );
        KiwiCon_PlaneToWorld( cplane, uv, out->position );
        out->valid = true;
        out->type  = SNAP_CPLANE;
        return true;
    }

    // ── arm 9: the ground-plane grid ─────────────────────────────────────────
    out->valid = true;
    out->type  = SNAP_GRID;
    if ( !KiwiGrid_Snap( raw, out->position ) )
    {
        // Unusable spacing — report the raw point rather than lying about a grid.
        out->type = SNAP_NONE;
    }
    return true;
}

// ─── marker (world space, inside the caller's open batch) ────────────────────
void KiwiSnap_EmitMarker( const snap_result_t &r )
{
    if ( !r.valid || !KiwiSnap_ShowMarkers() )
        return;

    camera_s *c = Ed_Camera();
    if ( c->width < 1 || c->height < 1 )
        return;

    // SHAKEOUT E: ONE near-black colour for every type — see KSNAP_COL_MARK.  The
    // per-rank tint moved to the LABEL, which is now the only thing that says
    // "vertex" vs "midpoint" vs "construction endpoint".
    KiwiLines_Color( KSNAP_COL_MARK[0], KSNAP_COL_MARK[1], KSNAP_COL_MARK[2] );

    const float wpp = WorldPerPixel( c, r.position );
    const float h   = wpp * 6.0f;
    float a[3], b[3];

    // POINT ranks — Plasticity's dot-plus-separated-ring.  ALL of them share it
    // now: vertex, midpoint, construction endpoint AND intersection.  The old "X"
    // for an intersection went with the per-type colours (the label still says
    // "intersection", which is where that information belongs).
    // SHAKEOUT F adds SNAP_FACE_CENTER to this list: it is ranked as a POINT
    // (kiwi_snap.h arm 4b), so it must LOOK like one — a glyph that says "area"
    // on a snap that behaves like a point is the marker lying about the rank.
    if ( r.type == SNAP_VERTEX || r.type == SNAP_EDGE_MID
      || r.type == SNAP_ENDPOINT || r.type == SNAP_INTERSECTION
      || r.type == SNAP_FACE_CENTER )
    {
        EmitDotAndRing( c, r.position, wpp );

        // A midpoint keeps its short tick ALONG the edge, so "mid" still reads as
        // "on that line" and not as "a vertex that appeared out of nowhere".  It
        // is the one piece of SHAPE differentiation worth keeping: it says where
        // the point is, not merely what kind it is.
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

    // v3 — the 15° lock: a camera-facing cross plus a longer tick ALONG the locked
    // direction, so "the direction is what snapped, not the point" reads at once.
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

    // (The POINT-rank arm — vertex / midpoint / endpoint / intersection — is the
    //  dot-and-ring block at the top of this function since shakeout E.  The
    //  camera-facing SQUARE it replaced is gone: the user asked for Plasticity's
    //  glyph, and two point glyphs would have been two answers to one question.)

    // LINE type — a short TICK along the edge, plus a small cross-tick so the
    // marker is visible when the edge runs straight at the camera.
    if ( r.type == SNAP_EDGE )
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

    // AREA type — a plain DOT: a tiny camera-facing diamond (4 segments), small
    // enough that it never competes with the point/line glyphs above.
    if ( r.type == SNAP_FACE )
    {
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

// ─── ROUND N: THE HOVERED-FACE SNAP ACCENTS ──────────────────────────────────
//
// USER DIRECTIVE, verbatim: "when the line tool(or similar) is in use, and a face
// is hovered, in plasticity (see pic) the points have an accent to help
// identifying snapping spots.  Add that."
//
// WHAT THE PICTURE SHOWS, and what this therefore is: while a drawing tool is
// live, the face under the cursor sprouts small dark dots at every place the tool
// could snap to — its corners, its edge midpoints and its centre — BEFORE the
// cursor is anywhere near them.  The existing marker only ever appears once you
// are already within 8 px of a target, which tells you what you HAVE snapped to
// and never what you COULD.  This is the "could".
//
// IT ADVERTISES EXACTLY THE SNAP SET, NOT A DECORATIVE ONE.  Every dot is a point
// KiwiSnap_Query will actually return for this face, and each is named by the arm
// that produces it: winding corners are arm 2 (SNAP_VERTEX), edge midpoints are
// arm 4 (SNAP_EDGE_MID), the centroid is arm 4b (SNAP_FACE_CENTER).  A dot the
// query could not honour would be worse than no dot at all.
//
// ── THE GATES, each with its reason ─────────────────────────────────────────
//   * a command must be LIVE and must want CLICKS (WantsClicks) — that is exactly
//     the "placement stage" set: the line/polyline/rect/circle/arc/spline tools and
//     the four solid primitives.  A DRAG gesture (G/R/S, a push/pull) is not
//     placing a point and would only gain clutter.
//   * the markers toggle is respected: this is the same feature as the marker and
//     must vanish with it.
//   * ONE face, the one under the cursor.  Not the brush, not the selection —
//     forty dots on one face is guidance, forty per brush is noise.
//
// ── THE BUDGET ──────────────────────────────────────────────────────────────
// KSNAP_ACCENT_MAX (40) dots, which is 12 corners + 12 midpoints + 1 centre for
// any face anyone will ever build (a brush face is convex and rarely past 8
// points), and each dot is a KSNAP_ACCENT_SEGS-gon with its diagonals filled —
// the same "make a line renderer draw a solid dot" trick EmitDisc already does for
// the marker.  Its OWN batch, sized from those two constants, because
// KiwiCmd_DrawWorld's 192 belongs to the command's own preview and the snap marker.
void KiwiSnap_DrawFaceAccents()
{
    if ( !KiwiSnap_ShowMarkers() )
        return;

    KiwiEditorCommand *cmd = KiwiCmd_Active();
    if ( !cmd || !cmd->WantsClicks() )
        return;

    camera_s *c = Ed_Camera();
    if ( c->width < 1 || c->height < 1 )
        return;

    int   x = 0, y = 0;
    ray_t ray;
    if ( !KiwiCmd_LastCursor( &x, &y ) || !Pick_RayFromImagePos( x, y, &ray ) )
        return;

    // SEL_MASK_FACE alone, and that is load-bearing: kiwi_pick.cpp only resolves an
    // area hit to a faceIndex when the FACE bit is set and the OBJECT bit is clear
    // (its `faceGranularity` test).  The mode mask is deliberately NOT consulted —
    // the accents are about where the TOOL can place a point, which does not change
    // with the selection mode.
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
    // The marker's own near-black ink, so a dot and the marker that lands on it
    // read as the same language.  No second colour: the ACCENT is smaller and the
    // marker gains its ring, which is the whole distinction.
    KiwiLines_Color( KSNAP_COL_MARK[0], KSNAP_COL_MARK[1], KSNAP_COL_MARK[2] );

    int   emitted = 0;
    float centre[3] = { 0.0f, 0.0f, 0.0f };

    for ( int i = 0; i < w->numpoints; ++i )
    {
        for ( int k = 0; k < 3; ++k )
            centre[k] += w->p[i][k];

        // A corner and the midpoint of the edge leaving it: two dots per winding
        // point, so one loop covers both target kinds and the budget is linear.
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

    // The centroid LAST, so a face busy enough to exhaust the budget still spends
    // it on the corners and midpoints — the targets a user actually aims at.
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

// ── ROUND Y, ITEM 3: ONE SPOT, FOR A TOOL THAT WANTS TO MARK A PLACE ────────
// USER DIRECTIVE, verbatim: "when using the split tool, show the center dot so I
// can find it easier."
//
// KiwiSnap_DrawFaceAccents cannot answer that: it gates on cmd->WantsClicks(),
// and the live split is a DRAG tool that confirms on RMB, so it never shows any
// accents at all.  Rather than relax that gate — which would put forty dots on
// every face during every drag gesture in the editor — the split draws the ONE
// spot it wants, in this file's glyph, through this entry point.
//
// Emits into whatever kiwi_lines batch is already OPEN (KiwiCmd_DrawWorld's, in
// the split's case), so the caller owns the budget and the colour run.  Sizes are
// in PIXELS at the point's own depth, like every other size in this layer.
void KiwiSnap_EmitSpot( const float *p, float pixRadius, bool solid )
{
    if ( !p || !( pixRadius > 0.0f ) )
        return;
    camera_s *c = Ed_Camera();
    if ( !c || c->width < 1 || c->height < 1 )
        return;
    float n[3];
    Nudge( c, p, n );
    EmitDisc( c, n, pixRadius * WorldPerPixel( c, p ),
              solid ? KSNAP_ACCENT_SEGS : KSNAP_RING_SEGS, solid );
}

// The two glyph sizes this file uses, published so a caller can match them
// exactly rather than copying numbers that then drift.
float KiwiSnap_AccentPixels() { return KSNAP_ACCENT_PIX; }
float KiwiSnap_RingPixels()   { return KSNAP_RING_PIX;   }

// ─── ROUND P: THE AXIS GUIDES (see kiwi_snap.h for the whole argument) ───────
//
// Drawn only when the LAST QUERY found an axis within its capture radius, so the
// guide appears the moment aiming starts to matter and never before.  Dashed by
// emitting a run of short segments — kiwi_lines has no dash mode and no alpha
// (TRAP 2), so "faint" is a dim GREY and "dashed" is geometry.
//
// ITS OWN BATCH, sized from the constants and bounded by construction: 32 dashes
// per side = 64 segments, plus the anchor dot's KSNAP_ACCENT_SEGS * 1.5 = 9, well
// inside KSNAP_AXIS_MAX_SEGS (96).  KiwiCmd_DrawWorld's own budget belongs to the
// command's preview and the snap marker, which is why this does not share it.
void KiwiSnap_DrawAxisGuides()
{
    if ( !s_axisHave || !KiwiSnap_ShowMarkers() )
        return;

    KiwiEditorCommand *cmd = KiwiCmd_Active();
    if ( !cmd || !cmd->WantsClicks() )
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

    // Symmetric about the anchor, so the two halves always look like one line
    // through the point rather than two patterns that happen to meet there.
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

    // Plasticity puts a dot on the source point (AxisSnap.ts:113-127) so it is
    // obvious WHICH point the axes are running through.  Same idea, same glyph
    // family as the accents.
    if ( KiwiLines_Remaining() >= KSNAP_ACCENT_SEGS + KSNAP_ACCENT_SEGS / 2 )
    {
        float p[3];
        Nudge( c, s_axisAnchor, p );
        EmitDisc( c, p, KSNAP_ACCENT_PIX * wpp, KSNAP_ACCENT_SEGS, true );
    }

    KiwiLines_Flush();
}

// ─── label (screen space, during the ImGui frame) ────────────────────────────
void KiwiSnap_DrawLabel( const snap_result_t &r, float imgMinX, float imgMinY,
                         float imgW, float imgH )
{
    if ( !r.valid || !KiwiSnap_ShowMarkers() )
        return;

    // §17: every number the user sees is inches, through the one conversion boundary.
    char bx[32], by[32], bz[32];
    KiwiUnits_Format( bx, sizeof( bx ), r.position[0] );
    KiwiUnits_Format( by, sizeof( by ), r.position[1] );
    KiwiUnits_Format( bz, sizeof( bz ), r.position[2] );

    char text[160];
    if ( r.type == SNAP_ANGLE && s_angleIsRel )
        // SHAKEOUT F: "rel" is the whole point of the readout — without it a user
        // cannot tell an absolute 45° from 45° off the wall they are drawing on.
        _snprintf( text, sizeof( text ), "%.0f deg rel   %s, %s, %s",
                   (double)s_angleRel, bx, by, bz );
    else if ( r.type == SNAP_ANGLE )
        _snprintf( text, sizeof( text ), "%.0f deg   %s, %s, %s",
                   (double)s_angleDeg, bx, by, bz );
    else if ( r.type == SNAP_AXIS )
        // ROUND P: the axis LETTER is the whole point of this label — it is what
        // tells the user they are on the vertical rather than merely near it.
        _snprintf( text, sizeof( text ), "%s axis   %s, %s, %s",
                   s_axisName, bx, by, bz );
    else
        _snprintf( text, sizeof( text ), "%s   %s, %s, %s",
                   KiwiSnap_TypeName( r.type ), bx, by, bz );
    text[sizeof( text ) - 1] = '\0';

    const ImVec2 sz = ImGui::CalcTextSize( text );
    const ImVec2 pad( 5.0f, 3.0f );

    // Anchored just below-right of the cursor, clamped inside the image.
    float x = imgMinX + (float)s_labelX + 16.0f;
    float y = imgMinY + (float)s_labelY + 14.0f;
    const float maxX = imgMinX + imgW - ( sz.x + pad.x * 2.0f ) - 2.0f;
    const float maxY = imgMinY + imgH - ( sz.y + pad.y * 2.0f ) - 2.0f;
    if ( x > maxX ) x = maxX;
    if ( y > maxY ) y = maxY;
    if ( x < imgMinX ) x = imgMinX;
    if ( y < imgMinY ) y = imgMinY;

    // Same rank→colour mapping as the world marker, so the label and the glyph
    // always agree about how strong a snap this is.
    ImU32 tint = IM_COL32( 220, 220, 235, 255 );                       // grid / off
    if ( r.type == SNAP_VERTEX || r.type == SNAP_EDGE_MID
      || r.type == SNAP_FACE_CENTER )                                  // shakeout F
        tint = IM_COL32( 120, 255, 145, 255 );                         // point
    else if ( r.type == SNAP_EDGE || r.type == SNAP_AXIS )             // ROUND P
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

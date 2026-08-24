#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_boxselect.cpp — RADIANT_UX_DESIGN §12 implementation.  See kiwi_boxselect.h.
//
// This is where Phase 1a's adapter goes LIVE for the first time: the marquee
// builds a selection_t and pushes it through Sel_SyncToLegacy(), so from here on
// the ported ops (hide, texture apply, clipper, CSG…) run on selections this new
// layer made.
//
// ── PROJECTION ───────────────────────────────────────────────────────────────
// Pick_WorldToImage is reused verbatim so the rect tests live in EXACTLY the space
// the cursor does.  It re-derives the basis per call, so a per-brush bounding-box
// pre-reject (8 corners instead of every winding point) keeps a whole-map marquee
// from turning into a per-vertex trig storm.  The pre-reject is conservative: the
// def's mins/maxs contain every winding point, so a projected-bbox AABB that misses
// the rect cannot contain a winding point that hits it — and any corner that fails
// to project (behind the eye) disables the reject for that brush.
//
// ── DEVIATION (flagged) ──────────────────────────────────────────────────────
// A PATCH has no brush windings, so its object test uses the control points: all
// inside == containment, any inside == crossing.  Control-net segments are not
// tested, so a crossing marquee that clips a patch strictly between two control
// points misses it.  Tessellated-mesh testing belongs with the patch work in
// Phase 3, not here.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"                     // brings qedefs.h — W_CAMERA / W_XY / W_Z
#include "mainfrm.h"                 // ROUND AG — camera_s (the facing gate + the preview)
#include "kiwi_boxselect.h"
#include "kiwi_camera.h"             // ROUND AJ, ITEM 1 — KiwiCam_WorldPerPixel
#include "kiwi_lines.h"              // ROUND AG, ITEM 6 — the live marquee preview
#include "kiwi_command.h"            // shakeout G — the face-click auto push/pull
#include "kiwi_conselect.h"          // shakeout F — the parallel construction selection
#include "kiwi_hover.h"
#include "kiwi_pick.h"
#include "kiwi_region.h"             // ROUND K — regions are clickable + extrudable
#include "kiwi_selection.h"
#include "kiwi_sun.h"                // the sun helper's glyph is clickable

#include <math.h>
#include <stdlib.h>                  // abs
#include <vector>

// ── ported entry points (verified against their definitions) ────────────────
// KIWI-UX (CLEANUP, C-55): no local `extern selbrush_t active_brushes;` — qe3.h
// (included above) declares both display-list sentinels, and the local copy also
// cited map.cpp, which is itself only an extern.
extern int        g_nUpdateBits;     // 0x25D5A74 (mainfrm.cpp)
// camwnd.cpp:157 camera_s *Ed_Camera(); :162 void CamWnd_BuildMatrix();
extern camera_s  *Ed_Camera();
extern void       CamWnd_BuildMatrix();

namespace
{
    struct rect2_t { float x0, y0, x1, y1; };

    // ── KIWI-UX (CLEANUP, C-55): the coincident-corner tolerance, NAMED ──────
    // Two faces of one brush name the SAME corner as two winding points, so a
    // marquee over a vertex would otherwise emit one SEL_VERTEX item per face that
    // touches it.  0.1 world units is a tenth of the finest useful grid — far below
    // anything a modeller places deliberately, far above float noise on a winding
    // rebuilt from planes.  kiwi_selconv.cpp names the SAME number KSC_TOL for the
    // SAME question; that one is TU-local (an anonymous-namespace const in a .cpp),
    // so it cannot be shared without promoting it to a header this cleanup does not
    // own — the two are a documented pair, not an accident.
    const float KBOX_COINCIDENT_TOL = 0.1f;

    bool  s_active   = false;
    bool  s_shift    = false;
    bool  s_ctrl     = false;
    int   s_startX   = 0, s_startY = 0;
    int   s_curX     = 0, s_curY   = 0;

    inline bool PointIn( const rect2_t &r, float x, float y )
    {
        return x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1;
    }

    // Liang-Barsky: does the segment a->b touch the rect at all?
    bool SegHitsRect( const rect2_t &r, float ax, float ay, float bx, float by )
    {
        float t0 = 0.0f, t1 = 1.0f;
        const float dx = bx - ax, dy = by - ay;
        const float p[4] = { -dx, dx, -dy, dy };
        const float q[4] = { ax - r.x0, r.x1 - ax, ay - r.y0, r.y1 - ay };
        for ( int i = 0; i < 4; ++i )
        {
            if ( p[i] == 0.0f )
            {
                if ( q[i] < 0.0f )
                    return false;                 // parallel and outside this edge
                continue;
            }
            const float t = q[i] / p[i];
            if ( p[i] < 0.0f )
            {
                if ( t > t1 ) return false;
                if ( t > t0 ) t0 = t;
            }
            else
            {
                if ( t < t0 ) return false;
                if ( t < t1 ) t1 = t;
            }
        }
        return true;
    }

    // ── per-shape accumulators ──────────────────────────────────────────────
    struct shapeTest_t
    {
        bool anyPoint   = false;   // at least one projected point inside the rect
        bool allInside  = true;    // every point projected AND was inside
        bool anySegment = false;   // some segment crosses the rect border
        bool started    = false;   // saw at least one point

        void Point( const rect2_t &r, bool projected, float x, float y )
        {
            started = true;
            if ( !projected ) { allInside = false; return; }
            if ( PointIn( r, x, y ) ) anyPoint = true;
            else                      allInside = false;
        }

        bool Contained() const { return started && allInside; }
        bool Crossed()   const { return anyPoint || anySegment; }
    };

    void TestWinding( const rect2_t &r, winding_t *w, shapeTest_t &t, bool wantSegments )
    {
        if ( !w || w->numpoints < 1 || w->numpoints > MAX_POINTS_ON_WINDING )
            return;
        // Project once, reuse for the point test and the segment test.  A brush face
        // never approaches MAX_POINTS_ON_WINDING; above the cache the segment test is
        // dropped (the point test still runs) rather than growing the stack.
        enum { CACHE = 64 };
        float cx[CACHE], cy[CACHE];
        bool  cok[CACHE];
        const int cached = ( w->numpoints < CACHE ) ? w->numpoints : CACHE;

        for ( int i = 0; i < w->numpoints; ++i )
        {
            float x = 0.0f, y = 0.0f;
            const bool ok = Pick_WorldToImage( w->p[i], &x, &y );
            if ( i < cached ) { cx[i] = x; cy[i] = y; cok[i] = ok; }
            t.Point( r, ok, x, y );
        }
        // Above the cache the segment test is skipped outright (wrapping at `cached`
        // would invent a closing edge); no brush face comes near 64 points.
        if ( !wantSegments || t.anySegment || w->numpoints < 2 || w->numpoints > cached )
            return;
        const int n = w->numpoints;
        for ( int i = 0; i < n; ++i )
        {
            const int j = ( i + 1 ) % n;
            if ( !cok[i] || !cok[j] )
                continue;
            if ( SegHitsRect( r, cx[i], cy[i], cx[j], cy[j] ) )
            {
                t.anySegment = true;
                return;
            }
        }
    }

    void TestPatchPoints( const rect2_t &r, patchMesh_t *pm, shapeTest_t &t )
    {
        // KIWI-UX (CLEANUP, C-55): the shared predicate, not a bare 16 —
        // Patch_DimsSane (kiwi_selection.h) is this exact test against
        // KIWI_PATCH_MAX_DIM.
        if ( !Patch_DimsSane( pm ) )
            return;
        for ( int col = 0; col < pm->width; ++col )
            for ( int row = 0; row < pm->height; ++row )
            {
                float x = 0.0f, y = 0.0f;
                const bool ok = Pick_WorldToImage( pm->ctrl[col][row].xyz, &x, &y );
                t.Point( r, ok, x, y );
            }
    }

    // Conservative whole-brush reject: the projected AABB of the def's bounding box.
    bool BrushMaybeInRect( const rect2_t &r, brush_t *def )
    {
        float lo[2] = {  1e30f,  1e30f };
        float hi[2] = { -1e30f, -1e30f };
        for ( int i = 0; i < 8; ++i )
        {
            const float p[3] = { ( i & 1 ) ? def->maxs[0] : def->mins[0],
                                 ( i & 2 ) ? def->maxs[1] : def->mins[1],
                                 ( i & 4 ) ? def->maxs[2] : def->mins[2] };
            float x, y;
            if ( !Pick_WorldToImage( p, &x, &y ) )
                return true;                       // straddles the eye plane — cannot reject
            if ( x < lo[0] ) lo[0] = x;
            if ( x > hi[0] ) hi[0] = x;
            if ( y < lo[1] ) lo[1] = y;
            if ( y > hi[1] ) hi[1] = y;
        }
        return !( hi[0] < r.x0 || lo[0] > r.x1 || hi[1] < r.y0 || lo[1] > r.y1 );
    }

    // The one granularity decision — see kiwi_boxselect.h.
    sel_kind_t RectKind( sel_mask_t mask )
    {
        if ( mask & SEL_MASK_OBJECT ) return SEL_OBJECT;
        if ( mask & SEL_MASK_FACE )   return SEL_FACE;
        if ( mask & SEL_MASK_EDGE )   return SEL_EDGE;
        return SEL_VERTEX;
    }

    // ═══════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND AI, ITEM 5) — THE TWO GATES ROUND AG LEFT OUT
    // ═══════════════════════════════════════════════════════════════════════
    // USER DIRECTIVE, verbatim: "when box selecting the faces, it shouldn't
    // penetrate through any brushes, just the visible faces.  also faces that are
    // only visible by 1 pixel (90 degrees facing left->right of the camera)
    // shouldn't be picked up either (So when I'm at a perfect south orientation, I
    // can box select every face on that south side without worrying about
    // east/west, etc.)"
    //
    // Round AG's facing gate said in as many words what it did not do — "it is a
    // FACING test, not an OCCLUSION test.  A front-facing face BEHIND a wall is
    // still collected" — and left it in KNOWN_ISSUES.  These are the two gates
    // that close it, and they answer different questions:
    //
    //   EDGE-ON is about PROJECTED AREA.  |n . d| where d is the eye->face
    //   direction IS the foreshortening factor: a face at 0.12 shows 12% of its
    //   true area, which at a perfect south view is exactly the east/west walls
    //   the user is complaining about.  This is a pure dot product, so it runs in
    //   the preview as well as the release and costs nothing.
    //
    //   OCCLUSION is about WHAT IS IN FRONT.  One ray from the eye to a witness
    //   point on the face; if the first thing it hits is a DIFFERENT brush, the
    //   face is behind something and is not what was pointed at.
    //
    // WHY THE OCCLUSION RAY ASKS FOR SEL_MASK_OBJECT AND COMPARES THE BRUSH, not
    // the face: an object query always resolves (kiwi_pick.cpp's face granularity
    // needs FACE-without-OBJECT and returns invalid on a patch or a model hit), so
    // a PATCH or a prefab standing in front of the face occludes it too — which a
    // face-granular query would have silently let through.  Comparing the BRUSH is
    // sufficient because the facing gate above has already dropped this brush's own
    // back faces, and a brush is convex by construction, so a ray from outside that
    // hits brush `b` at face f's centroid hits face f.
    //
    // FAIL OPEN.  When the ray hits NOTHING the face is ADMITTED, not rejected.
    // Test_Ray runs under the camera contents mask (Pick_CameraContents), so a
    // filtered content type — a tool brush, a trigger — can be un-hittable while
    // being perfectly visible and perfectly selectable, and a gate that rejected on
    // "no hit" would make those faces unmarqueeable.  The gate only ever rejects
    // when it positively identifies something ELSE in front.
    const float KBOX_FACE_EDGEON_DOT = 0.12f;

    // Extra witnesses tried only AFTER the centroid is found occluded, pulled
    // halfway from the centroid toward three spread winding corners.  A face whose
    // middle is behind a pillar but whose body is plainly visible would otherwise be
    // refused, and the cost profile is right: a VISIBLE face — the common case, and
    // the one the user is dragging over — still costs exactly one ray.
    enum { KBOX_FACE_WITNESSES = 3 };

    // Ray budgets.  The RELEASE is a one-shot gesture end and can afford to be
    // exact.  The PREVIEW re-runs on every 2-pixel rect change during the drag, so
    // it is capped — past the cap the gate is SKIPPED (admit), which makes the
    // preview a SUPERSET of the release rather than a subset.  That direction is
    // deliberate: the user may see one face highlighted that the release then drops,
    // but can never have something taken that was never shown.
    enum { KBOX_FACE_RAYS_RELEASE = 8192 };
    enum { KBOX_FACE_RAYS_PREVIEW = 96 };

    int  s_faceRayBudget = 0;

    // The witness ray.  Returns false only when something else is positively in
    // front of every witness tried.
    bool FaceUnoccluded( selbrush_t *b, winding_t *w, const float centroid[3] )
    {
        const camera_s *cam = Ed_Camera();
        for ( int wit = 0; wit <= KBOX_FACE_WITNESSES; ++wit )
        {
            if ( s_faceRayBudget <= 0 )
                return true;                       // budget spent — admit (see above)

            float p[3] = { centroid[0], centroid[1], centroid[2] };
            if ( wit > 0 )
            {
                // Spread the extra witnesses around the winding rather than taking
                // three adjacent corners, which on a long thin face would all sit at
                // one end of it.
                const int idx = ( w->numpoints * ( wit - 1 ) ) / KBOX_FACE_WITNESSES;
                if ( idx < 0 || idx >= w->numpoints )
                    continue;
                for ( int k = 0; k < 3; ++k )
                    p[k] = ( centroid[k] + w->p[idx][k] ) * 0.5f;
            }

            ray_t ray;
            float d[3];
            for ( int k = 0; k < 3; ++k )
            {
                ray.origin[k] = cam->origin[k];
                d[k]          = p[k] - cam->origin[k];
            }
            const float len = sqrtf( d[0] * d[0] + d[1] * d[1] + d[2] * d[2] );
            if ( !( len > 1.0e-3f ) )
                return true;                       // eye is ON the face — nothing to occlude it
            for ( int k = 0; k < 3; ++k )
                ray.dir[k] = d[k] / len;

            --s_faceRayBudget;
            const pick_result_t hit = Pick( ray, SEL_MASK_OBJECT );
            if ( !hit.valid || !Sel_ItemValid( hit.item ) )
                return true;                       // nothing hit — admit (fail open)
            if ( hit.item.brush == b )
                return true;                       // WE are the first thing along it
        }
        return false;                              // every witness was behind something
    }

    void CollectFromBrush( selbrush_t *b, const rect2_t &r, bool crossing,
                           sel_kind_t kind, std::vector<sel_item_t> &out )
    {
        brush_t *def = b->def;
        if ( !def )
            return;
        if ( !BrushMaybeInRect( r, def ) )
            return;

        // ── KIWI-UX (ROUND AZ, ITEM 1): A PATCH IN A FACE MARQUEE IS THE PATCH ──
        // USER DIRECTIVE, verbatim: "allow shift clicking of patch sides like
        // they're faces, so can they be textured at the same time as the faces."
        //
        // Round AM already made this ruling for the CLICK (kiwi_pick.cpp:641-669:
        // a patch has no faces, its symbiont brush is a bounding box, so the patch
        // ITSELF is the finest texturable thing there is and mode 3 resolves it as
        // SEL_OBJECT).  The MARQUEE never got the same ruling: the face/edge arm
        // below opens `if ( b->patch || !def->faces ) return;`, so a shift-DRAG
        // over a curve in mode 3 collected nothing while a shift-CLICK on the same
        // curve collected it.  Two gestures, one grammar — the drag now answers the
        // way the click does.
        //
        // IT DELEGATES TO THE OBJECT ARM RATHER THAN REIMPLEMENTING THE TEST, which
        // matters: that arm carries round AF's crossing-only bbox widen (the fillet
        // fix, :339-368) and `Contained()` vs `Crossed()` already mean the right
        // things there.  A second containment test written here would be the round-AF
        // bug again, in a new place.
        //
        // SEL_EDGE IS NOT INCLUDED.  A patch has no windings, so there is no edge to
        // name; edge mode keeps skipping patches exactly as before.
        const bool patchAsObject = ( kind == SEL_FACE ) && ( b->patch != nullptr );
        if ( kind == SEL_OBJECT || patchAsObject )
        {
            shapeTest_t t;
            if ( b->patch )
            {
                TestPatchPoints( r, def->patch, t );          // see the DEVIATION note
                // ── KIWI-UX (ROUND AF, ITEM 9): …AND THE BBOX, FOR CROSSING ────
                // USER REPORT, verbatim: "When hiding selected objects, q3 curves
                // don't hide. actually they aren't selectable at all. Fix this."
                //
                // The control-point-only test is honest about its limit at the top of
                // this file ("a crossing marquee that clips a patch strictly between
                // two control points misses it"), and for a fillet patch — three or
                // four control points strung along one edge — "strictly between two
                // control points" is MOST of the patch.  So a box drawn over a visible
                // curve selected nothing, which is indistinguishable from "patches are
                // not box-selectable".
                //
                // A patch brush carries a real convex bounding brush (AddBrushForPatch,
                // pmesh.cpp:840-884: Brush_Alloc + Brush_Create + Brush_BuildWindings),
                // so its `def->faces` windings exist and are exactly the box the patch
                // lives in.  Adding them to the SAME shapeTest widens the answer
                // without changing its shape: `Contained()` (the enclosing marquee)
                // lives in.
                //
                // IT IS ADDED ON THE CROSSING PATH ONLY, and that restriction is
                // load-bearing rather than cautious.  `Contained()` demands that EVERY
                // point fed to the test be inside the rect, and the bbox corners of a
                // diagonal patch stick out past its own control points — so feeding
                // them to an ENCLOSING marquee would make patches HARDER to box-select
                // than they were, which is the opposite of this item.  Crossing asks
                // "did the rect touch anything", where a wider witness set can only
                // ever say yes more often, and saying yes is the fix.
                if ( crossing && def->faces )
                    for ( int f = 0; f < def->faceCount; ++f )
                        TestWinding( r, def->faces[f].w, t, crossing );
            }
            else if ( def->faces )
                for ( int f = 0; f < def->faceCount; ++f )
                    TestWinding( r, def->faces[f].w, t, crossing );
            if ( crossing ? t.Crossed() : t.Contained() )
                out.push_back( Sel_MakeObject( b ) );
            return;
        }

        if ( kind == SEL_VERTEX )
        {
            if ( b->patch )
            {
                patchMesh_t *pm = def->patch;
                if ( !Patch_DimsSane( pm ) )     // KIWI-UX (CLEANUP, C-55)
                    return;
                for ( int col = 0; col < pm->width; ++col )
                    for ( int row = 0; row < pm->height; ++row )
                    {
                        float x, y;
                        if ( !Pick_WorldToImage( pm->ctrl[col][row].xyz, &x, &y ) )
                            continue;
                        if ( PointIn( r, x, y ) )
                            out.push_back( Sel_MakePatchPoint( b, col * pm->height + row ) );
                    }
                return;
            }
            if ( !def->faces )
                return;
            // Dedup coincident winding corners the way the ported vertex core does
            // (FindPoint's 0.1-unit test): one world corner shared by N faces must not
            // become N items.  Lowest faceIndex wins, matching kiwi_pick's rule.
            std::vector<const float *> seen;
            for ( int f = 0; f < def->faceCount; ++f )
            {
                winding_t *w = def->faces[f].w;
                if ( !w || w->numpoints < 1 || w->numpoints > MAX_POINTS_ON_WINDING )
                    continue;
                for ( int i = 0; i < w->numpoints; ++i )
                {
                    float x, y;
                    if ( !Pick_WorldToImage( w->p[i], &x, &y ) )
                        continue;
                    if ( !PointIn( r, x, y ) )
                        continue;
                    bool dup = false;
                    for ( size_t k = 0; k < seen.size() && !dup; ++k )
                        dup = fabsf( seen[k][0] - w->p[i][0] ) < KBOX_COINCIDENT_TOL
                           && fabsf( seen[k][1] - w->p[i][1] ) < KBOX_COINCIDENT_TOL
                           && fabsf( seen[k][2] - w->p[i][2] ) < KBOX_COINCIDENT_TOL;
                    if ( dup )
                        continue;
                    seen.push_back( w->p[i] );
                    out.push_back( Sel_MakeVertex( b, f, i ) );
                }
            }
            return;
        }

        // FACE / EDGE — brush windings only (patches have neither).
        if ( b->patch || !def->faces )
            return;
        // ROUND AG, ITEM 9: the eye, for the facing gate below.  Read here rather
        // than threaded through the signature — Ed_Camera returns the editor's one
        // camera record and cannot change inside a collect.
        const float *camOrigin = Ed_Camera()->origin;
        for ( int f = 0; f < def->faceCount; ++f )
        {
            winding_t *w = def->faces[f].w;
            if ( !w || w->numpoints < 1 || w->numpoints > MAX_POINTS_ON_WINDING )
                continue;

            if ( kind == SEL_FACE )
            {
                // ═══════════════════════════════════════════════════════════
                //  ROUND AG, ITEM 9 — THE FACING GATE
                // ═══════════════════════════════════════════════════════════
                // USER DIRECTIVE, verbatim: "Allow box selecting of faces on a
                // solid.  This is needed for complex shapes with lots of small
                // brushes."
                //
                // THE ARM ITSELF ALREADY EXISTED and has since round U: mode 3
                // sets SEL_MASK_FACE, RectKind (:186) answers SEL_FACE, and the
                // marquee walks every winding.  What made it unusable is that it
                // had NO FACING TEST, so a box over one wall took that wall's
                // face AND the face on the far side of the same brush AND both
                // faces of everything behind it.  On the "lots of small brushes"
                // geometry the directive is about, a single drag produced a
                // selection several times larger than what the user was pointing
                // at — which is indistinguishable from "it does not work".
                //
                // WHICH WITNESS: the EXISTING shapeTest over every winding point,
                // unchanged.  The brief offered "centroid, or any vertex — pick
                // one and justify"; the justification for picking NEITHER is that
                // the brush marquee's crossing/containment semantics are already
                // defined by this test (Contained = every point inside, Crossed =
                // any point inside or any edge crossing the border), and a face
                // marquee that answered a DIFFERENT question from the object
                // marquee in the same rect would be a second grammar.  A centroid
                // test would also silently refuse a big face the rect sits inside
                // — precisely what the centre-ray rescue in Collect() exists to
                // stop happening.
                //
                // WHAT THIS DOES NOT DO: it is a FACING test, not an OCCLUSION
                // test.  A front-facing face BEHIND a wall is still collected,
                // exactly as the object marquee still collects the brush behind
                // the wall — "match the brush marquee's semantics" is the rule,
                // and occluding would need a ray per face per frame.
                // ROUND AI, ITEM 5: it is not left undone any more — see the two
                // gates immediately below and the block comment on FaceUnoccluded.
                const float rel[3] = { camOrigin[0] - w->p[0][0],
                                       camOrigin[1] - w->p[0][1],
                                       camOrigin[2] - w->p[0][2] };
                const float *n = def->faces[f].plane.normal;
                if ( rel[0] * n[0] + rel[1] * n[1] + rel[2] * n[2] <= 0.0f )
                    continue;                   // back-facing: not what was pointed at

                // ── ROUND AI, ITEM 5, GATE 1: EDGE-ON ───────────────────────
                // The centroid is the witness for BOTH gates, so it is built once
                // here.  |n . d| is the face's foreshortening; below
                // KBOX_FACE_EDGEON_DOT the face projects to under 12% of its area
                // and is the "visible by 1 pixel" wall the directive names.
                float cen[3] = { 0.0f, 0.0f, 0.0f };
                for ( int i = 0; i < w->numpoints; ++i )
                {
                    cen[0] += w->p[i][0];
                    cen[1] += w->p[i][1];
                    cen[2] += w->p[i][2];
                }
                {
                    const float inv = 1.0f / (float)w->numpoints;
                    cen[0] *= inv; cen[1] *= inv; cen[2] *= inv;
                }
                {
                    float d[3] = { cen[0] - camOrigin[0],
                                   cen[1] - camOrigin[1],
                                   cen[2] - camOrigin[2] };
                    const float len = sqrtf( d[0] * d[0] + d[1] * d[1] + d[2] * d[2] );
                    if ( len > 1.0e-3f )
                    {
                        const float dot = ( d[0] * n[0] + d[1] * n[1] + d[2] * n[2] ) / len;
                        if ( fabsf( dot ) < KBOX_FACE_EDGEON_DOT )
                            continue;           // grazing: not what was pointed at
                    }
                }

                shapeTest_t t;
                TestWinding( r, w, t, crossing );
                if ( !( crossing ? t.Crossed() : t.Contained() ) )
                    continue;

                // ── ROUND AI, ITEM 5, GATE 2: OCCLUSION ─────────────────────
                // LAST, deliberately: it is the only gate that costs a ray, so it
                // only ever runs on a face the rect was actually going to take.
                if ( !FaceUnoccluded( b, w, cen ) )
                    continue;

                out.push_back( Sel_MakeFace( b, f ) );
                continue;
            }

            // SEL_EDGE: both endpoints inside (containment) / segment touches (crossing).
            const int n = w->numpoints;
            if ( n < 2 )
                continue;
            for ( int i = 0; i < n; ++i )
            {
                const int j = ( i + 1 ) % n;
                float ax, ay, bx, by;
                if ( !Pick_WorldToImage( w->p[i], &ax, &ay ) )
                    continue;
                if ( !Pick_WorldToImage( w->p[j], &bx, &by ) )
                    continue;
                const bool hit = crossing
                    ? SegHitsRect( r, ax, ay, bx, by )
                    : ( PointIn( r, ax, ay ) && PointIn( r, bx, by ) );
                if ( hit )
                    out.push_back( Sel_MakeEdge( b, f, i ) );
            }
        }
    }

    // ROUND AG, ITEM 6: split out so the LIVE PREVIEW can run the identical walk
    // without the centre-ray rescue below (one full Pick per call is worth it once
    // at the release and not once per drag frame — see KiwiBox_DrawPreview).
    // ROUND AI, ITEM 5: `rayBudget` is the occlusion gate's allowance for THIS
    // walk — KBOX_FACE_RAYS_RELEASE from the gesture end, KBOX_FACE_RAYS_PREVIEW
    // from the live preview.  It is set here rather than threaded through
    // CollectFromBrush's signature because it is a per-WALK quantity, not a
    // per-brush one, and every brush in one walk must draw on the same pool.
    void CollectNoRescue( const rect2_t &r, bool crossing, std::vector<sel_item_t> &out,
                          int rayBudget )
    {
        s_faceRayBudget = rayBudget;
        const sel_kind_t kind = RectKind( KiwiSel_GetModeMask() );
        selbrush_t *lists[2] = { &active_brushes, &selected_brushes };
        for ( int L = 0; L < 2; ++L )
        {
            selbrush_t *head = lists[L];
            // Sentinel walk: init from .next, advance via ->next.
            for ( selbrush_t *b = head->next; b && b != head; b = b->next )
            {
                if ( !Pick_BrushPickable( b ) )
                    continue;
                CollectFromBrush( b, r, crossing, kind, out );
            }
        }
    }

    void Collect( const rect2_t &r, bool crossing, std::vector<sel_item_t> &out )
    {
        const sel_kind_t kind = RectKind( KiwiSel_GetModeMask() );
        CollectNoRescue( r, crossing, out, KBOX_FACE_RAYS_RELEASE );   // ROUND AI, ITEM 5

        // A CROSSING rect that lies entirely inside one big face has no winding point
        // and no winding edge inside it, so every test above misses it — dragging a
        // small box in the middle of a wall would select nothing.  One ray through the
        // rect centre closes that hole for the area kinds.
        if ( crossing && ( kind == SEL_OBJECT || kind == SEL_FACE ) )
        {
            ray_t ray;
            if ( Pick_RayFromImagePos( (int)( ( r.x0 + r.x1 ) * 0.5f ),
                                       (int)( ( r.y0 + r.y1 ) * 0.5f ), &ray ) )
            {
                const pick_result_t hit =
                    Pick( ray, ( kind == SEL_OBJECT ) ? SEL_MASK_OBJECT : SEL_MASK_FACE );
                if ( hit.valid && Sel_ItemValid( hit.item ) )
                {
                    bool dup = false;
                    for ( size_t i = 0; i < out.size() && !dup; ++i )
                        dup = Sel_ItemEqual( out[i], hit.item );
                    if ( !dup )
                        out.push_back( hit.item );
                }
            }
        }
    }

    void ApplyAndSync( const std::vector<sel_item_t> &items, bool shift, bool ctrl )
    {
        selection_t &sel = KiwiSel();
        if ( !shift && !ctrl )
            Sel_Clear( sel );
        for ( size_t i = 0; i < items.size(); ++i )
        {
            if ( ctrl ) Sel_Remove( sel, items[i] );
            else        Sel_Add   ( sel, items[i] );
        }
        Sel_SyncToLegacy();
        g_nUpdateBits |= ( W_CAMERA | W_XY | W_Z );
    }

    // Construction selection still exposes an older toggle primitive.  Keep its
    // exact membership test here so the central click arm can enforce the shared
    // add/remove grammar without changing unrelated construction commands.
    bool ConItemSelected( const kconSelItem_t &item )
    {
        for ( int i = 0; i < KiwiConSel_Count(); ++i )
        {
            const kconSelItem_t *at = KiwiConSel_At( i );
            if ( at && at->object == item.object && at->kind == item.kind
              && at->index == item.index )
                return true;
        }
        return false;
    }

    // Ctrl only reaches Toggle when the exact item is already present, so it can
    // remove but never add.  Shift similarly only adds an absent item.
    void ApplyConClick( const kconSelItem_t &item, bool shift, bool ctrl )
    {
        const bool selected = ConItemSelected( item );
        if ( ctrl )
        {
            if ( selected )
                KiwiConSel_ApplyClick( item, false, true );
            return;
        }
        if ( shift )
        {
            if ( !selected )
                KiwiConSel_ApplyClick( item, true, false );
            return;
        }
        KiwiConSel_ApplyClick( item, false, false );
    }

    void ClickSelect( int imgX, int imgY, bool shift, bool ctrl )
    {
        // ── KIWI-UX: THE SUN HELPER'S GLYPH IS CLICKABLE ────────────────────
        // FIRST, above everything: the glyph is an aimed-at SCREEN target inside
        // KSUN_PICK_PIX (kiwi_sun.h), the same class of thing as a vertex or the
        // section ball, and it sits an orbit radius outside the map where no brush
        // competes for the pixel.  A hit takes the WHOLE click and leaves the brush
        // and construction selections exactly as they were — "I clicked the sun" is
        // not a statement about brushes, which is the ruling the construction and
        // region arms below already make for themselves.
        //
        const bool sunHit = KiwiSun_GlyphHit( imgX, imgY );
        if ( sunHit )
        {
            if ( ctrl )
            {
                if ( KiwiSun_Selected() )
                    KiwiSun_ClearSelection();
            }
            else
                KiwiSun_Select();
            g_nUpdateBits |= ( W_CAMERA | W_XY | W_Z );
            return;
        }
        // A plain click that did NOT land on the glyph drops the sun selection;
        // either set-preserving modifier leaves it alone.
        if ( !shift && !ctrl )
            KiwiSun_ClearSelection();

        ray_t ray;
        pick_result_t hit;
        if ( Pick_RayFromImagePos( imgX, imgY, &ray ) )
            hit = Pick( ray, KiwiSel_GetModeMask() );

        // ── KIWI-UX (shakeout F): CONSTRUCTION GEOMETRY IS CLICKABLE ─────────
        // USER REPORT: "Using 2(edge) you can't select lines. […] Lines aren't
        // selectable with any mode."  Both candidates are computed and compared in
        // SCREEN PIXELS; the rule is spelled out in full in kiwi_conselect.h, and
        // it is:
        //   * the brush pick missed                                  -> construction
        //   * the brush pick is an AREA hit (screenDist is 0 by      -> construction
        //     definition for SEL_FACE / SEL_OBJECT, kiwi_pick.cpp)
        //   * both are point/line hits, construction is closer       -> construction
        // A construction WIN takes the whole click: the brush selection is left
        // exactly as it was rather than being cleared, because "I clicked a
        // construction line" is not a statement about brushes.
        {
            kconSelItem_t conItem;
            float         conDist = 0.0f;
            const bool    conHit  = KiwiConSel_PickAt( imgX, imgY, &conItem, &conDist );
            if ( conHit )
            {
                const bool brushPointish = hit.valid
                                        && ( hit.item.kind == SEL_VERTEX
                                          || hit.item.kind == SEL_EDGE );
                const bool conWins = !hit.valid || !brushPointish
                                  || ( conDist < hit.screenDist );
                if ( conWins )
                {
                    ApplyConClick( conItem, shift, ctrl );
                    return;
                }
            }
            else if ( !shift && !ctrl )
            {
                // A plain click that did NOT land on construction geometry drops
                // the construction selection, the same way it drops the brush one
                // below.  Two selections, one click grammar.
                KiwiConSel_Clear();
            }
        }

        // ── KIWI-UX (ROUND K): A REGION IS CLICKABLE, AND IT EXTRUDES ────────
        // USER DIRECTIVE, verbatim: "Also I can't grab the light blue part as if
        // its a face.  It should be extrudable into a new solid(brush)."
        //
        // THE ARBITRATION, and every clause of it is a rule the user could state:
        //   * the ray must actually be inside the cell (KiwiRegion_PickAt does the
        //     even-odd test in plane space);
        //   * a BRUSH FACE that is CLOSER wins, because a region is a translucent
        //     film and the solid in front of it is a solid.  Measured as a
        //     ray-origin distance on both sides, so "closer" means the same thing
        //     for both;
        //   * a POINT-or-LINE brush hit (a vertex, an edge) wins OUTRIGHT at any
        //     depth.  Those are aimed-at targets inside a few pixels, exactly the
        //     rule the construction arm above already applies, and a region fill is
        //     an area hit competing with a point one.
        // A region WIN takes the whole click: the brush selection is left alone
        // rather than cleared, because "I clicked a region" is not a statement
        // about brushes — the same ruling the construction arm makes.
        //
        // Placed AFTER the construction arm on purpose: a construction LINE is the
        // boundary of the region it helps bound, and a click within the line's own
        // clickbox is a click on the line.
        // ── KIWI-UX (ROUND AA, ITEM 7): NOT IN OBJECT MODE ──────────────────
        // USER REPORT, verbatim: "Make pick mode 4 a brush-only pick mode."
        // A construction region face is construction geometry, so mode 4 must not
        // offer it either — "brush-only" means the brush and the entity, and this
        // arm is the other half of the gate that kiwi_conselect.cpp's
        // ModeAllowsConstructionLines put on lines.  Mode 3 KEEPS it: the report's
        // own words are "It should only be faces and construction faces", and this
        // is the construction face.  Exact equality against SEL_MASK_OBJECT for the
        // same reason as over there — mode 5 has the OBJECT bit too and must keep
        // picking everything.
        if ( KiwiSel_GetModeMask() != SEL_MASK_OBJECT )
        {
            ray_t rray;
            int   reg = -1;
            if ( Pick_RayFromImagePos( imgX, imgY, &rray ) )
                reg = KiwiRegion_PickAt( rray );
            if ( reg >= 0 )
            {
                const bool brushPointish = hit.valid
                                        && ( hit.item.kind == SEL_VERTEX
                                          || hit.item.kind == SEL_EDGE );
                bool  regionWins = !brushPointish;
                float regDist = 0.0f;
                if ( regionWins && hit.valid
                  && KiwiRegion_HitDistance( rray, reg, &regDist ) )
                {
                    const float dx = hit.point[0] - rray.origin[0];
                    const float dy = hit.point[1] - rray.origin[1];
                    const float dz = hit.point[2] - rray.origin[2];
                    const float brushDist = sqrtf( dx * dx + dy * dy + dz * dz );
                    if ( brushDist < regDist )
                        regionWins = false;
                }
                if ( regionWins )
                {
                    if ( !shift && !ctrl )
                        KiwiConSel_Clear();
                    // Regions expose a toggle primitive, but the click grammar is
                    // add/remove: call it only when that operation changes this one
                    // member.  Ctrl never clears the rest of the region set.
                    if ( ctrl )
                    {
                        if ( KiwiRegion_IsSelected( reg ) )
                            KiwiRegion_ToggleSelect( reg );
                    }
                    else if ( shift )
                    {
                        if ( !KiwiRegion_IsSelected( reg ) )
                            KiwiRegion_ToggleSelect( reg );
                    }
                    else
                        KiwiRegion_Select( reg );
                    g_nUpdateBits |= ( W_CAMERA | W_XY | W_Z );
                    // …and AUTO-ENTER the region extrude, PAUSED, exactly as a face
                    // click auto-enters push/pull.  Paused means the lollipop is up
                    // (kiwi_lollipop.h) and nothing follows the cursor until the ball
                    // is taken hold of, so the click that SELECTED the region cannot
                    // also pull a brush out of it.  Ctrl (the deselect modifier)
                    // starts nothing — there is nothing to extrude.
                    if ( !ctrl && !KiwiCmd_Active() )
                    {
                        if ( KiwiCmd_Start( KIWI_CMD_EXTRUDE_REGION ) )
                            KiwiCmd_Pause();
                    }
                    return;
                }
                // The region LOST (a brush was in front of it, or the click was
                // aimed at a vertex / edge).  A plain click still drops the region
                // selection, exactly as it drops the brush and construction ones —
                // "I clicked something else" means the same thing for all three.
                if ( !shift && !ctrl )
                    KiwiRegion_ClearSelection();
            }
            else if ( !shift && !ctrl )
            {
                KiwiRegion_ClearSelection();
            }
        }

        selection_t &sel = KiwiSel();
        if ( !hit.valid || !Sel_ItemValid( hit.item ) )
        {
            if ( shift || ctrl )
                return;                          // modified click on nothing: keep the selection
            Sel_Clear( sel );
        }
        else if ( ctrl )
        {
            Sel_Remove( sel, hit.item );
        }
        else if ( shift )
        {
            Sel_Add( sel, hit.item );
        }
        else
        {
            Sel_Clear( sel );
            Sel_Add( sel, hit.item );
        }
        Sel_SyncToLegacy();
        g_nUpdateBits |= ( W_CAMERA | W_XY | W_Z );

        // ── KIWI-UX (shakeout G): FACE MODE AUTO-ENTERS PUSH/PULL ────────────
        // USER DIRECTIVE, verbatim: "When selecting the face of a brush (3), it
        // should automatically enter the extrusion mode for that face."
        //
        // The command is the ORDINARY Move (KIWI_CMD_MOVE), which in a face context
        // IS the §20 push/pull — no second code path, no second undo shape, and
        // pressing G afterwards is a no-op rather than a second gesture.  It is
        // started PAUSED, so:
        //   * the gizmo appears (kiwi_gizmo.cpp draws it for whichever transform is
        //     active) with the shakeout-G face-NORMAL arrow on it,
        //   * nothing follows the cursor until a handle is actually grabbed
        //     (that is the other half of this round — see kiwi_transform.cpp
        //     Recompute), so the click that SELECTED the face cannot also nudge it,
        //   * RMB / Enter confirm, Esc cancels, exactly as every other gesture.
        //
        // NO UNDO RECORD FOR AN UNMOVED CANCEL, proven rather than asserted:
        // KiwiMoveCommand::Begin never mutates and never opens a bracket; the
        // bracket is opened by ApplyFaces, which is reached only from Recompute,
        // which is reached only from MouseMove, which KiwiCmd_MouseMove refuses to
        // deliver while PAUSED (kiwi_command.cpp).  And even once HOT, ApplyFaces
        // now returns before OpenUndoForBrushes while the scalar is still zero
        // (the shakeout-G first-mutation guard).  So Esc here runs Cancel ->
        // RestoreAll (a no-op) -> KiwiCmd_UndoCancel, which self-guards on a
        // bracket that was never opened.
        //
        // MODE 5 (EVERYTHING) DOES NOT AUTO-ENTER: the mask must be FACE and
        // nothing else.  In mode 5 a face click is one of four things the same
        // click could have meant, and starting a modal command off an ambiguous
        // pick is how a user loses a selection they were building.
        // Modified clicks (Shift-add / Ctrl-remove) do not auto-enter either —
        // those are selection-editing gestures by definition.
        if ( !shift && !ctrl
          && KiwiSel_GetModeMask() == SEL_MASK_FACE
          && hit.valid && hit.item.kind == SEL_FACE && Sel_ItemValid( hit.item )
          && !KiwiCmd_Active() )
        {
            if ( KiwiCmd_Start( KIWI_CMD_MOVE ) )
                KiwiCmd_Pause();
        }
    }
}

// ── ROUND K: the click grammar, exported (kiwi_boxselect.h says why) ────────
void KiwiBox_ClickSelectAt( int imgX, int imgY, bool shift, bool ctrl )
{
    ClickSelect( imgX, imgY, shift, ctrl );
}

// ── ROUND AA, ITEM 9: the rect's BRUSH pass, exported (kiwi_boxselect.h) ────
// Deliberately built out of the SAME two pieces KiwiBox_End's brush pass is —
// CollectFromBrush over both sentinel lists, plus the crossing centre-ray rescue —
// rather than out of Collect() itself, because Collect() resolves the granularity
// from KiwiSel_GetModeMask() and this one is pinned to SEL_OBJECT.  Everything
// else is the same code answering the same question.
int KiwiBox_CollectBrushes( int x0, int y0, int x1, int y1, bool crossing,
                            selbrush_t **out, int maxOut )
{
    if ( !out || maxOut <= 0 )
        return 0;

    rect2_t r;
    r.x0 = (float)( ( x0 < x1 ) ? x0 : x1 );
    r.x1 = (float)( ( x0 < x1 ) ? x1 : x0 );
    r.y0 = (float)( ( y0 < y1 ) ? y0 : y1 );
    r.y1 = (float)( ( y0 < y1 ) ? y1 : y0 );

    std::vector<sel_item_t> items;
    selbrush_t *lists[2] = { &active_brushes, &selected_brushes };
    for ( int L = 0; L < 2; ++L )
    {
        selbrush_t *head = lists[L];
        // Sentinel walk: init from .next, advance via ->next.
        for ( selbrush_t *b = head->next; b && b != head; b = b->next )
        {
            if ( !Pick_BrushPickable( b ) )
                continue;
            CollectFromBrush( b, r, crossing, SEL_OBJECT, items );
        }
    }

    // The same hole Collect() closes, for the same reason: a CROSSING rect drawn
    // entirely inside one big face touches no winding point and no winding edge.
    if ( crossing )
    {
        ray_t ray;
        if ( Pick_RayFromImagePos( (int)( ( r.x0 + r.x1 ) * 0.5f ),
                                   (int)( ( r.y0 + r.y1 ) * 0.5f ), &ray ) )
        {
            const pick_result_t hit = Pick( ray, SEL_MASK_OBJECT );
            if ( hit.valid && Sel_ItemValid( hit.item ) )
            {
                bool dup = false;
                for ( size_t i = 0; i < items.size() && !dup; ++i )
                    dup = Sel_ItemEqual( items[i], hit.item );
                if ( !dup )
                    items.push_back( hit.item );
            }
        }
    }

    int n = 0;
    for ( size_t i = 0; i < items.size() && n < maxOut; ++i )
        if ( items[i].brush )
            out[n++] = items[i].brush;
    return n;
}

// ─── gesture ─────────────────────────────────────────────────────────────────
void KiwiBox_Begin( int imgX, int imgY, bool shift, bool ctrl )
{
    s_active = true;
    s_shift  = shift;
    s_ctrl   = ctrl;
    s_startX = s_curX = imgX;
    s_startY = s_curY = imgY;
}

void KiwiBox_Update( int imgX, int imgY )
{
    if ( !s_active )
        return;
    if ( imgX == s_curX && imgY == s_curY )
        return;
    s_curX = imgX;
    s_curY = imgY;
    // ROUND AG, ITEM 6: the preview lives in the 3D pass, so the 3D pass has to
    // run.  Gated on the cursor actually having moved so a held-still marquee
    // costs nothing (the same discipline the re-collect throttle keeps).
    g_nUpdateBits |= W_CAMERA;
}

void KiwiBox_End( int imgX, int imgY )
{
    if ( !s_active )
        return;
    s_active = false;
    s_curX = imgX;
    s_curY = imgY;

    const int dx = s_curX - s_startX;
    const int dy = s_curY - s_startY;
    if ( abs( dx ) < KBOX_CLICK_PIXELS && abs( dy ) < KBOX_CLICK_PIXELS )
    {
        ClickSelect( s_startX, s_startY, s_shift, s_ctrl );
        return;
    }

    rect2_t r;
    r.x0 = (float)( ( dx < 0 ) ? s_curX : s_startX );
    r.x1 = (float)( ( dx < 0 ) ? s_startX : s_curX );
    r.y0 = (float)( ( dy < 0 ) ? s_curY : s_startY );
    r.y1 = (float)( ( dy < 0 ) ? s_startY : s_curY );

    const bool crossing = ( dx < 0 );            // right -> left (spec §12)

    std::vector<sel_item_t> items;
    Collect( r, crossing, items );

    // ═══════════════════════════════════════════════════════════════════════
    //  ROUND Y, ITEM 6 — THE SLIVER MARQUEE THAT ATE THE CLICK
    // ═══════════════════════════════════════════════════════════════════════
    // USER REPORT, verbatim: "Clicks are still ignored sometimes.  Makes it
    // really annoying to work fast."
    //
    // The click fallback above tests BOTH axes with &&, so a purely vertical
    // hand-wobble — dx = 0, dy = 9, the single most common click artifact and the
    // one a fast worker makes most — escapes it and runs a box select with a
    // ZERO-WIDTH rect.  From there everything is stacked against it:
    //   * crossing = ( dx < 0 ) is FALSE for a rightward or a zero dx, so the pass
    //     is CONTAINMENT, and no brush is ever contained in a 0 x 9 px rect
    //     (CollectFromBrush's OBJECT arm needs t.Contained());
    //   * the centre-ray rescue that exists for exactly this is gated on
    //     `crossing` (Collect, above), so it never runs;
    //   * ApplyAndSync then does Sel_Clear on the empty result.
    // So the click did nothing AND dropped the selection.  It is DIRECTIONAL —
    // a wobble that happens to travel left is crossing and is rescued by the
    // centre ray — which is exactly why it reads as "sometimes".
    //
    // THE FIX: a marquee that is a SLIVER on either axis and found NOTHING was a
    // click, and is re-run as one at the PRESS pixel.  Both conditions matter:
    //   * "sliver on either axis" keeps a deliberate thin crossing swipe (drag
    //     straight down a wall to catch a column of edges) working, because that
    //     gesture DOES find things;
    //   * "found nothing" is what makes this safe to apply to containment and
    //     crossing alike — a marquee that selected something is not a click by any
    //     reading, and this arm cannot take a selection away from one.
    // ClickSelect re-picks from scratch at the press pixel and covers construction
    // geometry and regions as well, so the recovered gesture is the full click
    // grammar rather than a brush-only consolation.
    if ( items.empty()
      && ( abs( dx ) < KBOX_CLICK_PIXELS || abs( dy ) < KBOX_CLICK_PIXELS ) )
    {
        ClickSelect( s_startX, s_startY, s_shift, s_ctrl );
        return;
    }

    ApplyAndSync( items, s_shift, s_ctrl );

    // KIWI-UX (shakeout F): the SAME rect against the construction store, at the
    // same granularity the mode asks for and with the same containment/crossing
    // rules.  It runs UNCONDITIONALLY rather than "only when the brush pass found
    // nothing" — a marquee is a statement about a screen region, and every kind of
    // thing inside that region is in it.  (Click-select is the one that has to
    // arbitrate, because a click names exactly one thing.)
    KiwiConSel_ApplyRect( r.x0, r.y0, r.x1, r.y1, crossing, s_shift, s_ctrl );
}

void KiwiBox_Cancel()
{
    s_active = false;
}

// ═════════════════════════════════════════════════════════════════════════════
//  ROUND AG, ITEM 6 — THE LIVE MARQUEE PREVIEW
// ═════════════════════════════════════════════════════════════════════════════
// USER DIRECTIVE, verbatim: "While box selecting, it should highlight the items
// in realtime as the box goes over them (quality of life)."
//
// PLASTICITY DOES THIS, and its own implementation is the argument for the
// throttle below: plasticity/src/selection/BoxSelection (the `onPointerMove`
// arm) re-runs its box intersection every move event and pushes the result into
// `selection.hovered`, i.e. the SAME hover collection a raycast writes to — so
// what the user sees mid-drag is the ordinary hover highlight, on a set.  KIWI
// does the same two things: re-run the SAME Collect() the release will run, and
// draw the result in the §18 hover cyan.  Reusing Collect is the whole point —
// a preview computed by a second, cheaper test would be a preview that lies, and
// a preview that lies about a selection is worse than none.
//
// ── THE COST, AND WHAT HOLDS IT ─────────────────────────────────────────────
// Collect is a full walk of both display lists with a per-brush bbox reject
// (BrushMaybeInRect), which is what makes it affordable at all.  On top of that:
//
//   * IT ONLY RE-COLLECTS WHEN THE RECT MOVED.  A mouse that is held still costs
//     nothing.  The threshold is KBOX_PREVIEW_EPS pixels ON ANY EDGE — a drag
//     that crawls one pixel at a time still updates (the comparison is against
//     the last COLLECTED rect, not the last frame's, so sub-threshold motion
//     accumulates and eventually trips it rather than being lost).
//   * THE CENTRE-RAY RESCUE IS SKIPPED.  Collect()'s tail fires one Pick() per
//     call, and that is a full scene ray — it is worth it once at the release
//     and it is not worth it per drag frame.  The consequence is honest and
//     small: a crossing rect entirely inside one big face previews nothing and
//     then selects that face on release.  Better that way round than a preview
//     that costs a raycast per mouse move.
//   * THE DRAW IS OUTLINES, NOT FILLS.  kiwi_hover.cpp's fill emitter is one
//     RC_DRAW_TRIS command PER FACE, which is right for one hovered item and
//     wrong for a marquee that can name a hundred.  Outlines all share one
//     budgeted line batch.
//   * ONE HARD SEGMENT BUDGET (KBOX_PREVIEW_SEGMENTS).  Past it the preview is
//     partial; the SELECTION is not, and never was — this pass reads state and
//     emits lines, and cannot change what the release does.
namespace
{
    enum { KBOX_PREVIEW_EPS      = 2 };     // px on any edge before a re-collect
    enum { KBOX_PREVIEW_SEGMENTS = 900 };   // the preview batch's hard cap

    std::vector<sel_item_t> s_preview;
    rect2_t                 s_previewRect  = { 0.0f, 0.0f, 0.0f, 0.0f };
    bool                    s_previewValid = false;

    inline bool RectMoved( const rect2_t &a, const rect2_t &b )
    {
        const float e = (float)KBOX_PREVIEW_EPS;
        return fabsf( a.x0 - b.x0 ) >= e || fabsf( a.x1 - b.x1 ) >= e
            || fabsf( a.y0 - b.y0 ) >= e || fabsf( a.y1 - b.y1 ) >= e;
    }

    // The winding outline of one item, nudged toward the eye exactly as
    // kiwi_hover.cpp's outline pass nudges (0.25 world units), so a preview line
    // on a brush face is not decided pixel-by-pixel against that face's depth.
    void PreviewWinding( const camera_s *c, winding_t *w )
    {
        if ( !w || w->numpoints < 2 || w->numpoints > MAX_POINTS_ON_WINDING )
            return;
        const float nudge = 0.25f;
        for ( int i = 0; i < w->numpoints; ++i )
        {
            const int j = ( i + 1 ) % w->numpoints;
            float a[3], b[3];
            for ( int k = 0; k < 3; ++k )
            {
                a[k] = w->p[i][k] - c->vpn[k] * nudge;
                b[k] = w->p[j][k] - c->vpn[k] * nudge;
            }
            if ( !KiwiLines_Add( a, b ) )
                return;                     // budget spent — partial, by design
        }
    }

    // ── KIWI-UX (ROUND AJ, ITEM 1): THE VERTEX PREVIEW ──────────────────────
    // USER REPORT, verbatim: "it's also impossible to edit more than 1."
    //
    // The rect's SEL_VERTEX arm has collected patch control points since it was
    // written (CollectFromBrush's patch branch, above) and the click grammar has
    // accumulated under Shift for just as long (Sel_Add in ClickSelect) — but the
    // PREVIEW drew nothing at those granularities, and the comment that used to
    // sit here justified that with "the ported vertex pass is already drawing"
    // those handles.  THAT IS NOT TRUE IN PATCH VERTEX MODE: the ported handle
    // builders walk `selected_brushes` and shakeout D removed the promotion that
    // put anything there (kiwi_patchverts.h KILL 3), so a user dragging a box over
    // a control lattice saw an empty rect sweep over the points and no highlight
    // whatsoever — from which the only available conclusion is "box select does
    // not work here, so I can only ever have one point".
    //
    // A vertex is four segments, and the rect that names N of them is exactly the
    // gesture whose result the user needs to see BEFORE releasing.  Edges are
    // still deliberately absent: a marquee at edge granularity names dozens of
    // brush edges the ported wireframe is genuinely already drawing.
    void PreviewPoint( const camera_s *c, const float *p )
    {
        const float h = KiwiCam_WorldPerPixel( p ) * 5.0f;
        const float nudge = 0.25f;
        float corner[4][3];
        const float sx[4] = { -1.0f,  1.0f,  1.0f, -1.0f };
        const float sy[4] = { -1.0f, -1.0f,  1.0f,  1.0f };
        for ( int i = 0; i < 4; ++i )
            for ( int k = 0; k < 3; ++k )
                corner[i][k] = p[k] + c->vright[k] * ( sx[i] * h )
                                    + c->vup[k]    * ( sy[i] * h )
                                    - c->vpn[k]    * nudge;
        for ( int i = 0; i < 4; ++i )
            if ( !KiwiLines_Add( corner[i], corner[( i + 1 ) & 3] ) )
                return;
    }

    // KIWI-UX (CLEANUP, C-49): PreviewVertexPos was the fourth copy of "the world
    // position of a SEL_VERTEX item"; it is Sel_ItemWorldPos (kiwi_selection.h)
    // now, which additionally bounds the winding at MAX_POINTS_ON_WINDING and
    // names the patch bound KIWI_PATCH_MAX_DIM.  The one call site passes
    // checkLive = false — the preview loop tests Sel_BrushLive on the same item
    // before entering the arm, and this runs once per previewed item per frame.
}

void KiwiBox_DrawPreview()
{
    if ( !s_active )
    {
        s_preview.clear();
        s_previewValid = false;
        return;
    }

    const int dx = s_curX - s_startX;
    const int dy = s_curY - s_startY;
    // Below the click threshold this is still a CLICK as far as KiwiBox_End is
    // concerned, so previewing a selection would be advertising the wrong verb.
    if ( abs( dx ) < KBOX_CLICK_PIXELS && abs( dy ) < KBOX_CLICK_PIXELS )
    {
        s_preview.clear();
        s_previewValid = false;
        return;
    }

    rect2_t r;
    r.x0 = (float)( ( dx < 0 ) ? s_curX : s_startX );
    r.x1 = (float)( ( dx < 0 ) ? s_startX : s_curX );
    r.y0 = (float)( ( dy < 0 ) ? s_curY : s_startY );
    r.y1 = (float)( ( dy < 0 ) ? s_startY : s_curY );
    const bool crossing = ( dx < 0 );

    if ( !s_previewValid || RectMoved( r, s_previewRect ) )
    {
        s_preview.clear();
        CollectNoRescue( r, crossing, s_preview, KBOX_FACE_RAYS_PREVIEW );   // ROUND AI, ITEM 5
        s_previewRect  = r;
        s_previewValid = true;
    }
    if ( s_preview.empty() )
        return;

    camera_s *c = Ed_Camera();
    if ( c->width < 1 || c->height < 1 )
        return;
    CamWnd_BuildMatrix();

    // ── KIWI-UX (ROUND AI, ITEM 4): WIDTH 2, NOT 1 ──────────────────────────
    // USER DIRECTIVE, verbatim: "The hover highlighting is too weak.  Should be the
    // same as when a brush is selected."  The preview has only ONE channel — an
    // outline — because a per-brush FILL over a marquee's worth of geometry is not
    // affordable (the fill emitter is one draw command per face, capped at 64 a
    // frame in kiwi_hover.cpp; a marquee routinely names more than that).  So the
    // one channel it does have has to carry the weight of the two a selection has,
    // and width 2 is the "grab me" weight DrawSelectedAccents already spends on the
    // same argument.  The segment budget is UNCHANGED — width is per batch, not per
    // segment, so this costs nothing.
    KiwiLines_Begin( KBOX_PREVIEW_SEGMENTS, 2 );
    // Share the raycast-hover palette: cyan for add/replace, warm for removal.
    float previewCol[3];
    KiwiHover_PreviewColor( s_ctrl, previewCol );
    KiwiLines_Color( previewCol[0], previewCol[1], previewCol[2] );
    for ( size_t i = 0; i < s_preview.size(); ++i )
    {
        const sel_item_t &it = s_preview[i];
        if ( !Sel_BrushLive( it.brush ) || !it.brush->def )
            continue;
        brush_t *def = it.brush->def;
        if ( it.kind == SEL_OBJECT )
        {
            if ( !def->faces )
                continue;
            for ( int f = 0; f < def->faceCount; ++f )
                PreviewWinding( c, def->faces[f].w );
        }
        else if ( it.kind == SEL_FACE )
        {
            if ( def->faces && it.faceIndex >= 0 && it.faceIndex < def->faceCount )
                PreviewWinding( c, def->faces[it.faceIndex].w );
        }
        else if ( it.kind == SEL_VERTEX )
        {
            // ROUND AJ, ITEM 1 — see PreviewPoint for why this arm had to exist.
            float p[3];
            if ( Sel_ItemWorldPos( it, p, false ) )
                PreviewPoint( c, p );
        }
        // EDGE previews stay deliberately absent: at that granularity the marquee
        // names dozens of brush edges the ported wireframe is already drawing, and
        // outlining each of them would be noise on top of noise.
        if ( KiwiLines_Remaining() <= 0 )
            break;
    }
    KiwiLines_Flush();
}

bool KiwiBox_Rect( int *x0, int *y0, int *x1, int *y1, bool *crossing )
{
    if ( !s_active )
        return false;
    if ( x0 ) *x0 = s_startX;
    if ( y0 ) *y0 = s_startY;
    if ( x1 ) *x1 = s_curX;
    if ( y1 ) *y1 = s_curY;
    if ( crossing ) *crossing = ( s_curX < s_startX );
    return true;
}

bool KiwiBox_RemovePreview()
{
    return s_active && s_ctrl;
}

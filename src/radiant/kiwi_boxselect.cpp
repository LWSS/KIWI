#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Directional marquee selection; see kiwi_boxselect.h.
// Rect tests use camera-RTT image space through Pick_WorldToImage.  The projected
// brush-bounds pre-reject is conservative; a failed corner projection disables it.
// Patch object tests use control points, so crossing between control points relies
// on the crossing-only bounding-brush fallback below.

#include "stdafx.h"
#include "qe3.h"                     // brings qedefs.h — W_CAMERA / W_XY / W_Z
#include "mainfrm.h"                 // camera_s
#include "kiwi_boxselect.h"
#include "kiwi_camera.h"             // KiwiCam_WorldPerPixel
#include "kiwi_lines.h"              // live marquee preview
#include "kiwi_command.h"            // face-click auto push/pull
#include "kiwi_conselect.h"          // construction selection
#include "kiwi_hover.h"
#include "kiwi_pick.h"
#include "kiwi_refimage.h"
#include "kiwi_region.h"             // region click/extrude
#include "kiwi_selection.h"
#include "kiwi_sun.h"                // the sun helper's glyph is clickable

#include <float.h>
#include <math.h>
#include <stdlib.h>                  // abs
#include <vector>

// Ported entry points.
extern int        g_nUpdateBits;     // 0x25D5A74 (mainfrm.cpp)
// camwnd.cpp:157 camera_s *Ed_Camera(); :162 void CamWnd_BuildMatrix();
extern camera_s  *Ed_Camera();
extern void       CamWnd_BuildMatrix();

namespace
{
    struct rect2_t { float x0, y0, x1, y1; };

    // Deduplicate shared winding corners at the ported FindPoint/KSC_TOL tolerance.
    // Units are world units: 0.1 is below the finest useful grid but above plane noise.
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
        // Cache projections for segment tests.  Above 64, keep point tests but skip
        // segments rather than inventing a closing edge or growing the stack.
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

    // Reject near-edge-on faces by foreshortening before testing occlusion.
    // Occlusion picks objects so patches and prefabs can block, then compares brush
    // identity.  A miss fails open because the camera contents mask can filter a
    // visible, selectable witness.
    const float KBOX_FACE_EDGEON_DOT = 0.12f;

    // After an occluded centroid, try three spread halfway-to-corner witnesses so a
    // face partly visible around an occluder is admitted; common visible faces cost one ray.
    enum { KBOX_FACE_WITNESSES = 3 };

    // Both caps fail open; the smaller preview cap can make its face set a superset
    // of the release set without spending release-sized ray budgets per recollect.
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

        // A patch has no faces, so face mode selects its finest texturable item: the
        // object.  Reuse object containment/crossing and its bbox fallback; edge mode
        // still skips patches because there is no winding edge to name.
        const bool patchAsObject = ( kind == SEL_FACE ) && ( b->patch != nullptr );
        if ( kind == SEL_OBJECT || patchAsObject )
        {
            shapeTest_t t;
            if ( b->patch )
            {
                TestPatchPoints( r, def->patch, t );          // control-net witnesses
                // For crossing only, add the convex bounding-brush windings so sparse
                // control points do not leave gaps.  Using them for containment would
                // wrongly reject diagonal patches whose projected bbox corners protrude.
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
                if ( !Patch_DimsSane( pm ) )     // fixed control-grid bound
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
        // One stable camera origin drives every facing test in this collect.
        const float *camOrigin = Ed_Camera()->origin;
        for ( int f = 0; f < def->faceCount; ++f )
        {
            winding_t *w = def->faces[f].w;
            if ( !w || w->numpoints < 1 || w->numpoints > MAX_POINTS_ON_WINDING )
                continue;

            if ( kind == SEL_FACE )
            {
                // Drop back-facing faces before the marquee test.  The full winding,
                // not its centroid, preserves object-style containment/crossing and
                // leaves wholly enclosed crossing faces to Collect's center-ray rescue.
                const float rel[3] = { camOrigin[0] - w->p[0][0],
                                       camOrigin[1] - w->p[0][1],
                                       camOrigin[2] - w->p[0][2] };
                const float *n = def->faces[f].plane.normal;
                if ( rel[0] * n[0] + rel[1] * n[1] + rel[2] * n[2] <= 0.0f )
                    continue;                   // back-facing: not what was pointed at

                // Share the centroid between foreshortening and occlusion.  A dot
                // below 0.12 is treated as an edge-on face.
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

                // Ray last: only a face already inside the marquee pays for occlusion.
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

    // Preview shares the release walk but omits the center-ray rescue.  rayBudget
    // is one pool for the whole walk rather than a per-brush allowance.
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
        CollectNoRescue( r, crossing, out, KBOX_FACE_RAYS_RELEASE );   // release budget

        // A crossing rect wholly inside a large face touches no winding point or edge.
        // One center ray closes that hole for area kinds.
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
        // The screen-space sun glyph arbitrates before geometry and consumes the
        // click; its selection is independent of the geometry selection stores.
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

        // Construction candidates compete in screen pixels.  They win on a brush
        // miss or area hit; against a brush vertex/edge, the closer point/line wins.
        // A construction win consumes the click without clearing brush selection.
        bool constructionCandidate = false;
        {
            kconSelItem_t conItem;
            float         conDist = 0.0f;
            const bool    conHit  = KiwiConSel_PickAt( imgX, imgY, &conItem, &conDist );
            constructionCandidate = conHit;
            if ( conHit )
            {
                const bool brushPointish = hit.valid
                                        && ( hit.item.kind == SEL_VERTEX
                                          || hit.item.kind == SEL_EDGE );
                const bool conWins = !hit.valid || !brushPointish
                                  || ( conDist < hit.screenDist );
                if ( conWins )
                {
                    if ( !shift && !ctrl )
                        KiwiRefImage_Select( -1 );
                    ApplyConClick( conItem, shift, ctrl );
                    return;
                }
            }
            else if ( !shift && !ctrl )
            {
                // With no construction candidate, a plain click clears that store.
                KiwiConSel_Clear();
            }
        }

        // Reference images are object/area targets.  Any brush vertex/edge or any
        // construction candidate keeps priority.  Against a brush face/object area
        // hit, compare eye distance and let the nearer surface own the click.
        if ( !constructionCandidate
          && ( KiwiSel_GetModeMask() == SEL_MASK_OBJECT
            || KiwiSel_GetModeMask() == SEL_MASK_EVERYTHING ) )
        {
            int image = -1;
            float imageDist = FLT_MAX;
            if ( KiwiRefImage_PickAt( imgX, imgY, &image, &imageDist ) )
            {
                const bool brushPointish = hit.valid
                                        && ( hit.item.kind == SEL_VERTEX
                                          || hit.item.kind == SEL_EDGE );
                bool imageWins = !hit.valid || !Sel_ItemValid( hit.item );
                if ( !imageWins && !brushPointish )
                {
                    const float dx = hit.point[0] - ray.origin[0];
                    const float dy = hit.point[1] - ray.origin[1];
                    const float dz = hit.point[2] - ray.origin[2];
                    const float brushDist = sqrtf( dx * dx + dy * dy + dz * dz );
                    imageWins = !( brushDist < imageDist );
                }
                if ( imageWins )
                {
                    KiwiRefImage_ApplyClick( image, shift, ctrl, true );
                    return;
                }
            }
        }

        // Regions are area targets: a nearer brush face wins, while brush vertices
        // and edges always win.  Construction arbitrates first because its lines
        // bound regions.  Mode 4 is brush-only; mode 5 remains unrestricted.
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
                    {
                        KiwiConSel_Clear();
                        KiwiRefImage_Select( -1 );
                    }
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
                    // Auto-enter paused extrusion so the selecting click cannot move
                    // geometry; Ctrl removes and therefore starts nothing.
                    if ( !ctrl && !KiwiCmd_Active() )
                    {
                        if ( KiwiCmd_Start( KIWI_CMD_EXTRUDE_REGION ) )
                            KiwiCmd_Pause();
                    }
                    return;
                }
                // A plain click won by brush geometry drops the region selection.
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

        // In face-only mode, a plain face click auto-enters the ordinary Move command,
        // whose face context is push/pull.  Start paused so selection cannot move the
        // face; undo opens on first mutation.  Mode 5 and modified clicks stay nonmodal.
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

// Exported click grammar; see kiwi_boxselect.h.
void KiwiBox_ClickSelectAt( int imgX, int imgY, bool shift, bool ctrl )
{
    ClickSelect( imgX, imgY, shift, ctrl );
}

// Export the brush pass for object-only tools.  It mirrors CollectFromBrush over
// both sentinel lists plus crossing center-ray rescue, but pins SEL_OBJECT instead
// of resolving the current selection mode.
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

    // Match Collect's center-ray rescue for crossing rects wholly inside a face.
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
    // Preview renders in the camera pass; invalidate it only after actual motion.
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

    // A near-zero axis can escape the two-axis click threshold yet yield an empty
    // containment marquee.  If the brush pass found nothing, retry at the press as
    // a click; requiring an empty result preserves thin drags that select brushes.
    if ( items.empty()
      && ( abs( dx ) < KBOX_CLICK_PIXELS || abs( dy ) < KBOX_CLICK_PIXELS ) )
    {
        ClickSelect( s_startX, s_startY, s_shift, s_ctrl );
        return;
    }

    ApplyAndSync( items, s_shift, s_ctrl );

    // Apply the rect independently to construction geometry: unlike a click, a
    // marquee can legitimately name both brush and construction items.
    KiwiConSel_ApplyRect( r.x0, r.y0, r.x1, r.y1, crossing, s_shift, s_ctrl );
    // KIWI (REFIMG, 2026-09-03): reference images are NEVER box-selected - a marquee over
    // a traced photo must select the geometry on it, not the photo.  Click / outliner only.
}

void KiwiBox_Cancel()
{
    s_active = false;
}

// Live preview reuses the brush collect walk.  Recollect after a two-pixel edge
// delta; smaller motion accumulates against the last collected rect.  Skip the
// full-scene center ray, so a crossing rect inside one large face may preview empty
// then select it on release.  Draw outlines in one capped batch; partial rendering
// never changes the release selection.
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

    // Preview candidates are not selected yet, so draw vertex markers explicitly.
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
        CollectNoRescue( r, crossing, s_preview, KBOX_FACE_RAYS_PREVIEW );   // preview budget
        s_previewRect  = r;
        s_previewValid = true;
    }
    if ( s_preview.empty() )
        return;

    camera_s *c = Ed_Camera();
    if ( c->width < 1 || c->height < 1 )
        return;
    CamWnd_BuildMatrix();

    // Width 2 matches selected accents because preview has no fill channel.  Width
    // is batch state and does not consume the segment budget.
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
            float p[3];
            // The loop already checked liveness, so skip the duplicate test.
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

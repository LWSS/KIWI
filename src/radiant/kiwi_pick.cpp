#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Unified pick implementation; coordinate and index conventions are in kiwi_pick.h.
// Surface hits use the ported Test_Ray path; vertices and edges rank projected
// brush geometry in screen pixels. Picking never mutates map geometry.

#include "stdafx.h"
#include <universal/assertive.h>
#include "qe3.h"
#include "mainfrm.h"     // camera_s
#include "prefs.h"       // g_PrefsDlg (the Test_Ray contents gates + Fov)
#include "kiwi_camera.h" // orthographic ray/projection helpers
#include "kiwi_pick.h"
#include "kiwi_section.h"   // section-plane pick clamps
#include "kiwi_vec.h"     // Dot3/Sub3/...

#include <math.h>
#include <vector>

// ── ported entry points (verified against their definitions) ────────────────
extern camera_s  *Ed_Camera();                                     // camwnd.cpp
extern void       CamWnd_BuildMatrix();                            // camwnd.cpp 0x403470
extern void       Ed_CameraCalcRayDir( int x, int y, float *dir ); // camwnd.cpp forwarder
// Orthographic rays need per-pixel image-plane origins; perspective returns camera.origin.
extern void       Ed_CameraCalcRayOrigin( int x, int y, float *org ); // camwnd.cpp forwarder
extern void       Test_Ray( float *start, float *dir, int contents,
                            edTrace_t *t, int num_traces );        // select.cpp 0x48D7C0
extern char       FilterBrush( selbrush_t *b, int updateFilters ); // filters.cpp 0x46A1F0
extern selbrush_t active_brushes;                                  // map.cpp    (0x23F189C)
extern entity_s  *world_entity;                                    // map.cpp    (0x25D5B30)
extern bool       ImGuiShell_CameraPaintCursor( int *x, int *y, int *w, int *h ); // imgui_shell.cpp

namespace
{
    // brushFlags bit 5 mirrors sub_48D460's ordinary-pick exclusion; only the
    // 0x1000 pass admits it, then SelectFaceSth rejects it. qedefs.h has no symbol.
    const int BRUSHFLAG_PICK_EXCLUDED = 0x20;


    // Projection constants are latched once per pick; `s` matches CameraCalcRayDir's
    // per-pixel scale. `ortho` and `pivotDist` must stay paired with that transform.
    struct projCtx_t
    {
        const camera_s *cam       = nullptr;
        float           s         = 0.0f;
        int             w         = 0;
        int             h         = 0;
        bool            ortho     = false;
        float           pivotDist = 0.0f;   // ortho only: the constant divisor
        bool            ok        = false;
    };

    projCtx_t MakeProjCtx()
    {
        projCtx_t p;
        p.cam = Ed_Camera();
        p.w   = p.cam->width;
        p.h   = p.cam->height;
        if ( p.w < 1 || p.h < 1 )
            return p;
        // Identical to CameraCalcRayDir: s = (t*0.75 + t*0.75) / height.
        const double t = tan( DEG2RAD( g_PrefsDlg->camera_fov ) * 0.5 );
        p.s = (float)( ( t * 0.75 + t * 0.75 ) / (double)p.h );
        if ( !( p.s > 0.0f ) )
            return p;
        // Match Ed_CameraCalcRayOrigin's orthographic recovery: dist = H / tanY.
        p.ortho = KiwiCam_Ortho();
        if ( p.ortho )
        {
            const float tanY = (float)( t * 0.75 );
            if ( !( tanY > 1.0e-6f ) )
                return p;
            p.pivotDist = KiwiCam_OrthoHalfHeight() / tanY;
            if ( !( p.pivotDist > 1.0e-6f ) )
                return p;
        }
        p.ok = true;
        return p;
    }

    // World → TOP-LEFT camera-image pixels, exactly inverse to CameraCalcRayDir.
    // Keep integer w/2 and h/2; ortho divides by pivotDist to invert its lateral ray
    // origin and deliberately ignores the arbitrary eye plane along the view axis.
    bool ProjectRaw( const projCtx_t &p, const float *world, float *ox, float *oy )
    {
        if ( !p.ok )
            return false;
        const camera_s *c = p.cam;
        const float rel[3] = { world[0] - c->origin[0],
                               world[1] - c->origin[1],
                               world[2] - c->origin[2] };
        float div;
        if ( p.ortho )
        {
            div = p.pivotDist;
        }
        else
        {
            div = Dot3( rel, c->vpn );
            if ( div <= 0.001f )                // at or behind the eye plane
                return false;
        }
        const float xf = Dot3( rel, c->vright ) / div;
        const float yf = Dot3( rel, c->vup ) / div;
        const float xb = xf / p.s + (float)( p.w / 2 );
        const float yb = yf / p.s + (float)( p.h / 2 );
        *ox = xb;
        *oy = (float)( p.h - 1 ) - yb;          // undo the shell's height - y - 1 flip
        return true;
    }

    // Retain the local name while sharing the canonical implementation in kiwi_pick.h.
    inline float SegDist2D( float px, float py, float ax, float ay,
                            float bx, float by, float *outT )
    {
        return Pick_SegDist2D( px, py, ax, ay, bx, by, outT );
    }

    // Mirrors sub_48D460's world-pass admission. Prefab/model interiors are excluded:
    // their windings are prefab-local and would project as world coordinates;
    // Test_Ray resolves them at object granularity.
    bool BrushPickable( selbrush_t *b )
    {
        if ( !b || !b->def )
            return false;
        if ( FilterBrush( b, 0 ) )
            return false;
        if ( ( b->brushFlags & BRUSHFLAG_PICK_EXCLUDED ) != 0 )
            return false;
        entity_s *owner = b->owner;
        if ( !owner )
            return false;
        if ( owner != world_entity )
        {
            entity_s *ownerDef = owner->def;
            if ( !ownerDef || !ownerDef->eclass )
                return false;
            if ( ( ownerDef->eclass->classtype & ECLASS_PREFAB ) != 0 )
                return false;
        }
        return true;
    }

    // Snapshot once because KiwiSel() may rebuild selection state; never call it
    // from the candidate scan. Linear membership assumes selections stay small.
    struct excludeSet_t
    {
        std::vector<const selbrush_t *> nodes;

        void Build()
        {
            for ( selbrush_t *b = selected_brushes.next;
                  b && b != &selected_brushes; b = b->next )
                Add( b );
            const selection_t &sel = KiwiSel();
            for ( size_t i = 0; i < sel.items.size(); ++i )
                Add( sel.items[i].brush );
        }

        void Add( const selbrush_t *b )
        {
            if ( !b )
                return;
            for ( size_t i = 0; i < nodes.size(); ++i )
                if ( nodes[i] == b )
                    return;
            nodes.push_back( b );
        }

        bool Has( const selbrush_t *b ) const
        {
            for ( size_t i = 0; i < nodes.size(); ++i )
                if ( nodes[i] == b )
                    return true;
            return false;
        }
    };

    // Test_Ray's ported walker skips BRUSHFLAG_SELECTED, so ordinary picks temporarily
    // clear the bit without touching selection counters. End() restores recorded bits.
    // Exclusion picks stay masked; rejecting their nearest hit cannot expose one behind.
    struct selUnmask_t
    {
        std::vector<selbrush_t *> nodes;

        void Begin()
        {
            for ( selbrush_t *b = selected_brushes.next;
                  b && b != &selected_brushes; b = b->next )
            {
                if ( ( b->brushFlags & BRUSHFLAG_SELECTED ) != 0 )
                {
                    b->brushFlags &= ~(int)BRUSHFLAG_SELECTED;
                    nodes.push_back( b );
                }
            }
        }

        void End()
        {
            for ( size_t i = 0; i < nodes.size(); ++i )
                nodes[i]->brushFlags |= (int)BRUSHFLAG_SELECTED;
            nodes.clear();
        }

        ~selUnmask_t() { End(); }
    };

    struct pickBest_t
    {
        bool       hit  = false;
        float      dist = 0.0f;
        sel_item_t item;
        float      point[3] = { 0.0f, 0.0f, 0.0f };
    };

    void Consider( pickBest_t &best, float dist, float tol,
                   const sel_item_t &item, const float *point )
    {
        if ( dist > tol )
            return;
        if ( best.hit && !( dist < best.dist ) )   // strict <: first candidate wins ties
            return;
        best.hit  = true;
        best.dist = dist;
        best.item = item;
        best.point[0] = point[0];
        best.point[1] = point[1];
        best.point[2] = point[2];
    }

    // Screen-space vertex/edge scan over one display list.
    void ScanList( selbrush_t *sentinel, const projCtx_t &p, sel_mask_t kindMask,
                   float curX, float curY, pickBest_t &bestVert, pickBest_t &bestEdge,
                   const excludeSet_t *exclude )
    {
        const bool wantVert = ( kindMask & SEL_MASK_VERTEX ) != 0;
        const bool wantEdge = ( kindMask & SEL_MASK_EDGE ) != 0;

        for ( selbrush_t *b = sentinel->next; b && b != sentinel; b = b->next )
        {
            if ( !BrushPickable( b ) )
                continue;
            if ( exclude && exclude->Has( b ) )
                continue;

            brush_t *def = b->def;

            // Patch grids expose control points only; grid edges are not brush windings,
            // and terrain dragging addresses control points.
            if ( b->patch )
            {
                if ( !wantVert )
                    continue;
                // Mixed object masks resolve patches as SEL_OBJECT; dense control-point
                // hit discs would otherwise swallow the area pick and block texturing.
                // Vertex-only patch mode still scans the control points.
                if ( ( kindMask & SEL_MASK_OBJECT ) != 0 )
                    continue;
                patchMesh_t *pm = def->patch;
                if ( !pm || pm->width <= 0 || pm->height <= 0
                  || pm->width > 16 || pm->height > 16 )
                    continue;
                for ( int col = 0; col < pm->width; ++col )
                {
                    for ( int row = 0; row < pm->height; ++row )
                    {
                        const float *xyz = pm->ctrl[col][row].xyz;
                        float sx, sy;
                        if ( !ProjectRaw( p, xyz, &sx, &sy ) )
                            continue;
                        const float dx = sx - curX, dy = sy - curY;
                        Consider( bestVert, sqrtf( dx * dx + dy * dy ), PICK_VERT_PIXELS,
                                  Sel_MakePatchPoint( b, col * pm->height + row ), xyz );
                    }
                }
                continue;
            }

            if ( !def->faces || def->faceCount <= 0 )
                continue;

            for ( int f = 0; f < def->faceCount; ++f )
            {
                winding_t *w = def->faces[f].w;
                if ( !w )
                    continue;
                int n = w->numpoints;
                if ( n < 1 || n > MAX_POINTS_ON_WINDING )
                    continue;

                // Cache 64 projected points; MAX_POINTS_ON_WINDING (1024) is too large
                // for stack pair arrays, so unusually large faces reproject beyond it.
                enum { CACHE = 64 };
                float cx[CACHE], cy[CACHE];
                bool  cok[CACHE];
                const int cached = ( n < CACHE ) ? n : CACHE;
                for ( int i = 0; i < cached; ++i )
                    cok[i] = ProjectRaw( p, w->p[i], &cx[i], &cy[i] );

                if ( wantVert )
                {
                    for ( int i = 0; i < n; ++i )
                    {
                        float sx, sy;
                        if ( i < cached )
                        {
                            if ( !cok[i] ) continue;
                            sx = cx[i]; sy = cy[i];
                        }
                        else if ( !ProjectRaw( p, w->p[i], &sx, &sy ) )
                            continue;
                        const float dx = sx - curX, dy = sy - curY;
                        Consider( bestVert, sqrtf( dx * dx + dy * dy ), PICK_VERT_PIXELS,
                                  Sel_MakeVertex( b, f, i ), w->p[i] );
                    }
                }

                if ( wantEdge && n >= 3 )
                {
                    for ( int i = 0; i < n; ++i )
                    {
                        const int j = ( i + 1 ) % n;
                        float ax, ay, bx, by;
                        if ( i < cached ) { if ( !cok[i] ) continue; ax = cx[i]; ay = cy[i]; }
                        else if ( !ProjectRaw( p, w->p[i], &ax, &ay ) ) continue;
                        if ( j < cached ) { if ( !cok[j] ) continue; bx = cx[j]; by = cy[j]; }
                        else if ( !ProjectRaw( p, w->p[j], &bx, &by ) ) continue;

                        float t = 0.0f;
                        const float d = SegDist2D( curX, curY, ax, ay, bx, by, &t );
                        const float mid[3] = {
                            w->p[i][0] + ( w->p[j][0] - w->p[i][0] ) * t,
                            w->p[i][1] + ( w->p[j][1] - w->p[i][1] ) * t,
                            w->p[i][2] + ( w->p[j][2] - w->p[i][2] ) * t };
                        Consider( bestEdge, d, PICK_EDGE_PIXELS,
                                  Sel_MakeEdge( b, f, i ), mid );
                    }
                }
            }
        }
    }
}

// Share admission with box selection so its candidate set cannot drift from clicks.
bool Pick_BrushPickable( selbrush_t *b )
{
    return BrushPickable( b );
}

// ─── ray construction ────────────────────────────────────────────────────────
bool Pick_RayFromImagePos( int imgX, int imgY, ray_t *out )
{
    if ( !out )
        return false;
    camera_s *c = Ed_Camera();
    if ( c->width < 1 || c->height < 1 )
        return false;

    // Rebuild the camera basis so picks issued between draws do not use stale vectors.
    CamWnd_BuildMatrix();

    // Flip against camera_s.height, which CamWnd_RenderToRT keeps aligned with the RTT.
    // Both forwarders are required so orthographic rays get per-pixel origins.
    const int flipY = c->height - imgY - 1;
    Ed_CameraCalcRayOrigin( imgX, flipY, out->origin );
    Ed_CameraCalcRayDir   ( imgX, flipY, out->dir );
    return true;
}

bool Pick_RayFromCursor( ray_t *out )
{
    int x, y;
    if ( !ImGuiShell_CameraPaintCursor( &x, &y, nullptr, nullptr ) )
        return false;
    return Pick_RayFromImagePos( x, y, out );
}

bool Pick_WorldToImage( const float *world, float *outX, float *outY )
{
    if ( !world || !outX || !outY )
        return false;
    CamWnd_BuildMatrix();
    const projCtx_t p = MakeProjCtx();
    return ProjectRaw( p, world, outX, outY );
}

int Pick_CameraContents()
{
    // Drag_Begin (drag.cpp 0x47E890) for viewz == 2, minus the 0x4000 light-preview
    // bit (that path diverts to the per-light preview, not a selection pick).
    int contents = 0;
    if ( g_PrefsDlg->entities_off )        contents  = 0x200;
    if ( g_PrefsDlg->m_bSelectableModels ) contents |= 0x400;
    if ( g_PrefsDlg->sky_brush_off )       contents |= 0x800;
    contents |= 0x1000;                    // the 3D camera view is viewz 2
    return contents;
}

// ─── the pick ────────────────────────────────────────────────────────────────
pick_result_t Pick( const ray_t &ray, sel_mask_t kindMask, unsigned pickFlags )
{
    pick_result_t r;
    if ( !kindMask )
        return r;

    CamWnd_BuildMatrix();
    const projCtx_t p = MakeProjCtx();

    // Built before anything else so both passes see the same set (kiwi_pick.h).
    excludeSet_t excl;
    const excludeSet_t *pExcl = nullptr;
    if ( ( pickFlags & PICKF_EXCLUDE_SELECTED ) != 0 )
    {
        excl.Build();
        pExcl = &excl;
    }

    // ── screen-space pass (verts, then edges) ────────────────────────────────
    if ( p.ok && ( kindMask & ( SEL_MASK_VERTEX | SEL_MASK_EDGE ) ) != 0 )
    {
        // The cursor pixel IS the ray: project a point one unit down it.  This is
        // exact (round-trips CameraCalcRayDir) and keeps Pick() ray-driven rather
        // than needing a second cursor source.
        const float ahead[3] = { ray.origin[0] + ray.dir[0],
                                 ray.origin[1] + ray.dir[1],
                                 ray.origin[2] + ray.dir[2] };
        float curX, curY;
        if ( ProjectRaw( p, ahead, &curX, &curY ) )
        {
            pickBest_t bestVert, bestEdge;
            ScanList( &active_brushes,   p, kindMask, curX, curY, bestVert, bestEdge, pExcl );
            ScanList( &selected_brushes, p, kindMask, curX, curY, bestVert, bestEdge, pExcl );

            // Point beats line beats area (spec §6's ranking, applied to picking).
            const pickBest_t &win = bestVert.hit ? bestVert : bestEdge;
            // Test the closest screen-space candidate against the section plane; a
            // hidden winner falls through to the clamped area pass. Testing only the
            // winner avoids a plane test per candidate and is a no-op without a cut.
            if ( win.hit && KiwiSection_PointVisible( win.point ) )
            {
                r.valid      = true;
                r.item       = win.item;
                r.point[0]   = win.point[0];
                r.point[1]   = win.point[1];
                r.point[2]   = win.point[2];
                r.screenDist = win.dist;
                return r;
            }
        }
    }

    // ── area pass: the ported Test_Ray chain, unmodified ─────────────────────
    if ( ( kindMask & ( SEL_MASK_FACE | SEL_MASK_OBJECT ) ) == 0 )
        return r;

    float start[3] = { ray.origin[0], ray.origin[1], ray.origin[2] };
    float dir[3]   = { ray.dir[0],    ray.dir[1],    ray.dir[2]    };
    // If sectioning hides the ray origin, advance it to the plane before Test_Ray.
    // Every Pick() area hit passes here; the clamp is inactive without a cut or
    // when the origin is already visible.
    KiwiSection_ClampRayStart( start, dir );
    const int contents = Pick_CameraContents();

    edTrace_t t;
    {
        // The destructor restores recorded selected bits before later early returns.
        selUnmask_t unmask;
        if ( !pExcl )
            unmask.Begin();
        Test_Ray( start, dir, contents, &t, 1 );
    }
    selbrush_t *hb = t.hit.brush;
    if ( !hb )
        return r;

    // PICKF_EXCLUDE_SELECTED, area arm: Test_Ray keeps ONE nearest hit, so an
    // excluded surface simply means "no surface under the cursor" (kiwi_pick.h).
    if ( pExcl && pExcl->Has( hb ) )
        return r;

    // SelectFaceSth's 0x1000 (camera/Z view) rejection, reproduced: a flag-0x20
    // brush, or an inner prefab brush the 0x1000 pass admitted (t._pad[0]).
    if ( ( contents & 0x1000 ) != 0
      && ( ( hb->brushFlags & BRUSHFLAG_PICK_EXCLUDED ) != 0 || t._pad[0] ) )
        return r;

    r.valid    = true;
    r.point[0] = start[0] + dir[0] * t.dist;
    r.point[1] = start[1] + dir[1] * t.dist;
    r.point[2] = start[2] + dir[2] * t.dist;
    r.screenDist = 0.0f;                       // area hit (spec §2)
    // Copy only a usable unit normal so haveNormal consumers need not renormalise.
    {
        const float l2 = t.normal[0] * t.normal[0] + t.normal[1] * t.normal[1]
                       + t.normal[2] * t.normal[2];
        if ( l2 > 0.9f && l2 < 1.1f )
        {
            r.normal[0]  = t.normal[0];
            r.normal[1]  = t.normal[1];
            r.normal[2]  = t.normal[2];
            r.haveNormal = true;
        }
    }

    // Face granularity requires FACE without OBJECT; mixed masks use classic object
    // behavior. Patches use object granularity; prefab/model internals must not
    // resolve to editable faces.
    const bool faceGranularity = ( kindMask & SEL_MASK_FACE ) != 0
                              && ( kindMask & SEL_MASK_OBJECT ) == 0;
    int faceIndex = -1;
    if ( faceGranularity && !hb->patch && hb->faces && t.hit.face )
    {
        const int idx = (int)( t.hit.face - hb->faces );
        if ( idx >= 0 && idx < hb->faceCount )
            faceIndex = idx;
    }

    if ( faceIndex >= 0 )
        r.item = Sel_MakeFace( hb, faceIndex );
    else if ( ( kindMask & SEL_MASK_OBJECT ) != 0 )
        r.item = Sel_MakeObject( hb );
    // A patch has no editable faces; its symbiont brush is only a bounding box, so
    // face-only mode returns the patch object rather than a bogus box face.
    // Prefab/model internals are intentionally unsupported in face-only mode.
    else if ( hb->patch )
        r.item = Sel_MakeObject( hb );
    else
        r.valid = false;                       // face-only mask, no resolvable face

    return r;
}

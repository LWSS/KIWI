#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_pick.cpp — RADIANT_UX_DESIGN §2 implementation.  See kiwi_pick.h for the
// coordinate conventions and the edge/vertex indexing scheme.
//
// NEW code over the ported cores: the surface pick IS Test_Ray (unmodified); the
// vertex/edge pick is a screen-space rank over the brush lists the editor already
// walks to draw.  Nothing here mutates map data.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include <universal/assertive.h>
#include "qe3.h"
#include "mainfrm.h"     // camera_s
#include "prefs.h"       // g_PrefsDlg (the Test_Ray contents gates + Fov)
#include "kiwi_camera.h" // ROUND M: KiwiCam_Ortho / KiwiCam_OrthoHalfHeight
#include "kiwi_pick.h"
#include "kiwi_section.h"   // KIWI-UX (ROUND BM) — the section plane clamps picks
#include "kiwi_vec.h"     // KIWI-UX (CLEANUP, A-15): the one spelling of Dot3/Sub3/...

#include <math.h>
#include <vector>

// ── ported entry points (verified against their definitions) ────────────────
extern camera_s  *Ed_Camera();                                     // camwnd.cpp
extern void       CamWnd_BuildMatrix();                            // camwnd.cpp 0x403470
extern void       Ed_CameraCalcRayDir( int x, int y, float *dir ); // camwnd.cpp (KIWI-UX forwarder)
// ROUND M: the ORIGIN half of the ray, so the ortho arm's parallel rays start on
// the image plane instead of at the eye.  Perspective: returns camera.origin
// unchanged, so this is a no-op rename of what Pick_RayFromImagePos already did.
extern void       Ed_CameraCalcRayOrigin( int x, int y, float *org ); // camwnd.cpp (KIWI-UX)
extern void       Test_Ray( float *start, float *dir, int contents,
                            edTrace_t *t, int num_traces );        // select.cpp 0x48D7C0
extern char       FilterBrush( selbrush_t *b, int updateFilters ); // filters.cpp 0x46A1F0
extern selbrush_t active_brushes;                                  // map.cpp    (0x23F189C)
extern entity_s  *world_entity;                                    // map.cpp    (0x25D5B30)
extern bool       ImGuiShell_CameraPaintCursor( int *x, int *y, int *w, int *h ); // imgui_shell.cpp

namespace
{
    // selbrush_t.brushFlags bit 5 — the "excluded from the ordinary pick" gate the
    // ported walker sub_48D460 tests (admitted only on the 0x1000 pass, and then
    // rejected again by SelectFaceSth).  No symbol exists for it in qedefs.h.
    const int BRUSHFLAG_PICK_EXCLUDED = 0x20;


    // Per-pick projection constants, latched once so the candidate loop does not
    // re-derive tan(fov) per point.  `s` is CameraCalcRayDir's per-pixel scale.
    //
    // ROUND M: `ortho` / `pivotDist` carry the orthographic arm.  See camwnd.cpp
    // Ed_CameraCalcRayDir for the full FORWARD/INVERSE pair this must match — the
    // two are useless apart, so they are written down in one place and referenced
    // from the other.
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
        // ROUND M: dist = H / tanY — the SAME recovery Ed_CameraCalcRayOrigin
        // uses, so the ray builder and this inverse share one number.
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

    // World → camera-image pixels (TOP-LEFT origin).  Exact inverse of
    // CameraCalcRayDir: that builds dir = vpn + vright*xf + vup*yf with
    // xf = (x - w/2)*s, yf = (y - h/2)*s, so a point at origin + k*dir satisfies
    // dot(rel,vpn) = k, dot(rel,vright) = k*xf, dot(rel,vup) = k*yf.  The integer
    // halves (w/2, h/2) are kept integer for the same reason.
    //
    // ROUND M (ORTHO): the divisor becomes the CONSTANT pivot distance instead of
    // the point's own depth — that one substitution is the whole difference, and
    // it is the exact inverse of the ortho ray's lateral ORIGIN offset (camwnd.cpp
    // Ed_CameraCalcRayOrigin).  The depth test also changes meaning: in ortho a
    // point behind the eye PLANE is still on screen (the eye point is arbitrary
    // along the view axis), so only the ±KCAM_ORTHO_DEPTH slab could reject it and
    // nothing an editor holds reaches that — hence no rejection here at all.
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

    // KIWI-UX (CLEANUP, A-12): this body moved to kiwi_pick.h as
    // Pick_SegDist2D — it was the canonical copy of a loop five other files had
    // open-coded.  The local name is kept as a one-line forwarder so this file's
    // call sites read as they always did.
    inline float SegDist2D( float px, float py, float ax, float ay,
                            float bx, float by, float *outT )
    {
        return Pick_SegDist2D( px, py, ax, ay, bx, by, outT );
    }

    // The candidate filter, mirroring sub_48D460's admission rules for the world
    // pass.  Prefab/model interiors are excluded on purpose: spec §2 resolves those
    // to SEL_OBJECT via Test_Ray, and their brushes live in prefab-local space, so
    // their windings would project to the wrong pixels.
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

    // ── PICKF_EXCLUDE_SELECTED (Phase 3) ────────────────────────────────────
    // Snapshotted ONCE per Pick() call rather than tested per candidate against
    // the live selection: the scan is O(brushes) and the set is O(selection), so
    // building it up front turns an O(brushes * selection) walk with a KiwiSel()
    // call inside the inner loop into one build plus a small linear membership
    // test.  (KiwiSel() can force a rebuild — never call it inside the scan.)
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

    // ═════════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND X, ITEMS 5 + 8) — A SELECTED BRUSH IS INVISIBLE TO Test_Ray.
    // ═════════════════════════════════════════════════════════════════════════
    // USER REPORTS, verbatim:
    //   (5) "If a brush is selected and while its selected, I switch to mode-3 and
    //        select a face, it takes 2 clicks. Fix this so it just works."
    //   (8) "Sometimes when clicking, an object isn't selected and it takes 2 tries.
    //        No clue why. This also happens with faces."
    //
    // ONE CAUSE, and it is not click slop.  `sub_48D460` — the ported brush-list
    // walker Test_Ray runs over BOTH lists (select.cpp:653-742) — skips outright:
    //
    //     int brushflags = sbn->brushFlags;                     // select.cpp:675
    //     if ( ( brushflags & BRUSHFLAG_SELECTED ) != 0 )       // select.cpp:677
    //         continue;
    //
    // and `Brush_Select_Helper` (brush.cpp:844-846, called by Brush_AddToList2
    // BEFORE it links the node into selected_brushes) sets exactly that bit.  So
    // the area pass CANNOT hit anything that is already selected — the selected
    // list is walked and then every member of it is thrown away.
    //
    // The user-visible shape is precisely "two clicks":
    //   click 1 — the only brush under the cursor is the selected one, Test_Ray
    //             reports nothing, and ClickSelect's miss arm clears the selection;
    //   click 2 — the brush is no longer selected, so now it picks.
    // In face mode that is report (5) exactly, and in object mode it is report (8):
    // re-clicking the thing you already had selected drops it instead of keeping it.
    //
    // THE FIX, and why it lives here rather than in select.cpp: the walker's skip is
    // ported behaviour and stays.  What ROUND X changes is the STATE it is asked to
    // walk — the bit is lifted for the duration of the one Test_Ray call and put
    // straight back.  It is synchronous, it allocates nothing that can throw between
    // the two halves, no drawing or message pump runs inside it, and the selection
    // COUNTERS are untouched (the bit is flipped directly, never through
    // Brush_Select_Helper / Brush_Deselect_Helper, which are what own d_select_count).
    //
    // Keeping a selected object selectable is also the standard behaviour and
    // Plasticity's: a plain click on an already-selected item makes it the selection
    // rather than dropping it, and Ctrl is what removes it.
    //
    // NOT APPLIED under PICKF_EXCLUDE_SELECTED: that flag means "pretend the
    // selection is not there", so unmasking would only produce a nearest hit that
    // the exclude test below then rejects — hiding the surface BEHIND it, which is
    // the hazard already logged for that flag's area arm.
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

        // Sentinel walk: init from .next, advance via ->next.
        for ( selbrush_t *b = sentinel->next; b && b != sentinel; b = b->next )
        {
            if ( !BrushPickable( b ) )
                continue;
            if ( exclude && exclude->Has( b ) )
                continue;

            brush_t *def = b->def;

            // ── patch brushes: control points only (v1; grid edges are not brush
            //    windings and the terrain-drag core addresses control points).
            if ( b->patch )
            {
                if ( !wantVert )
                    continue;
                // ── KIWI-UX (ROUND AK, ITEM 3) — A PATCH'S CONTROL POINTS NEED
                //    VERTEX GRANULARITY, NOT JUST THE VERTEX BIT ────────────────
                // USER REPORT, verbatim: "bevel is good, but I can't select the
                // curve parts to retexture them."
                //
                // THE CHAIN, AND IT IS NOT ANY OF THE FOUR GATES ROUND AF FOUND.
                // Those were all audited again this round and every one of them
                // already defaults permissive: m_bSelectCurves is 1 (prefs.cpp:82
                // and :191), the Curve/Terrain filters default isShown = true
                // (filters.cpp:982, registry default 1 at :1091), and the mode mask
                // starts at SEL_MASK_EVERYTHING (kiwi_selection.cpp:55).  The fifth
                // cause is HERE:
                //   * mode 5 (the default) has BOTH the VERTEX and the OBJECT bits;
                //   * this screen-space pass runs BEFORE the Test_Ray area pass
                //     (kiwi_pick.cpp, the "screen-space pass" block below) and a
                //     hit RETURNS IMMEDIATELY, so the area pass never runs;
                //   * this branch accepts ANY control point within PICK_VERT_PIXELS
                //     (8 px) — and a fillet arc is (spans*2+1) x 3 control points
                //     packed into one chamfer (kiwi_patchfillet.cpp:1272-1273), so
                //     at working zoom the 8 px discs TILE the whole visible patch;
                //   * so every click on a fillet resolved to a SEL_VERTEX item, and
                //     Sel_SyncToLegacy pushes only SEL_OBJECT onto selected_brushes
                //     (kiwi_selection.cpp:352 — shakeout D removed the promotion),
                //     so Brush_SetTexture early-returned on an empty selection
                //     (select.cpp:1792) and the Textures-panel click was a SILENT
                //     NO-OP.  Both halves of the report, one chain.
                // It bites FILLETS and not large patches for exactly one reason:
                // control-point density per screen pixel.
                //
                // THE RULE IS THE ONE THIS FILE ALREADY APPLIES TO FACES: granularity
                // means the bit is set AND the OBJECT bit is not (see faceGranularity
                // in Pick(), which reads `(kindMask & SEL_MASK_FACE) && !(kindMask &
                // SEL_MASK_OBJECT)`).  Under it:
                //   mode 5 / 4  -> the PATCH is picked, and can be textured;
                //   patch vertex mode (V) sets SEL_MASK_VERTEX ALONE
                //     (kiwi_patchverts.cpp:370, and :286 asserts that ownership),
                //     so its control points still pick exactly as before.
                // SCOPED TO PATCHES ON PURPOSE: brush vertices in mode 5 are a
                // handful of corner handles, not a tiling field, and they are a
                // gesture the editor has always had at that mask.  Nothing outside
                // this `if ( b->patch )` branch changes.
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

                // Project the winding once; reuse for both verts and edges.
                // MAX_POINTS_ON_WINDING is 1024 — too big for a stack array of
                // pairs here, so cap the projected cache at a brush-face-sane 64
                // (a convex brush face never approaches that) and fall back to
                // per-edge reprojection above it.
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

// KIWI-UX (Phase 1b): export the admission filter unchanged, so kiwi_boxselect.cpp
// admits exactly the brushes a click pick would instead of carrying a second copy
// that can drift.  Pure forwarder — no behaviour change to the pick itself.
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

    // The ported picker relies on the draw having run Cam_BuildMatrix; recompute it
    // so a pick issued before/between draws still gets a valid basis.  Pure function
    // of camera.angles — the values are identical to the ones the draw computes.
    CamWnd_BuildMatrix();

    // The shell's flip base is camera_s.height (kept in step with the ImGui dock
    // cell by CamWnd_RenderToRT), the same expression CamWnd_OnLButtonDown uses.
    // ROUND M: BOTH halves come from the camwnd forwarders now, so the ortho arm's
    // parallel rays get their per-pixel origin instead of the eye.
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
            // KIWI-UX (ROUND BM, ITEM 1b): a SECTION must not let the user click a
            // vertex or an edge it has cut away.  The winner is tested rather than
            // every candidate — one plane test per pick instead of one per vertex —
            // and a hidden winner FALLS THROUGH to the area pass below, which is
            // clamped to the same plane.  kiwi_section.h carries the argument and
            // the one case where the two differ.  No-op while no section is on.
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
    // KIWI-UX (ROUND BM, ITEM 1b): with a SECTION armed, a ray that begins in the
    // hidden half is advanced to the section plane, so Test_Ray cannot return a
    // surface the user cannot see.  This is the ONE place it has to happen — every
    // pick in the editor (hover, selection, the snap query, every command, the
    // camera's own pivot and dolly references) funnels through this function.
    // No-op while no section is on, and no-op for a ray that already starts in the
    // visible half.  kiwi_section.h states why the visible-side case needs nothing.
    KiwiSection_ClampRayStart( start, dir );
    const int contents = Pick_CameraContents();

    edTrace_t t;
    {
        // KIWI-UX (ROUND X, ITEMS 5 + 8) — see selUnmask_t above.  The destructor
        // restores the bit on every exit from this block, including the early
        // returns that follow it once `unmask` has gone out of scope.
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

    // Face granularity only when the mask asks for faces and NOT objects: with both
    // bits set (mode 5 "Everything") an area hit resolves to the whole object, which
    // is classic Radiant's LMB behaviour.  Patches and prefab/model hits are always
    // SEL_OBJECT (spec §2: no editing inside prefab instances in v1).
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
    // ── KIWI-UX (ROUND AM, ITEM 7a) — A PATCH IN FACE MODE IS THE PATCH ──────
    // USER REPORT, verbatim: "I still cant face select a curve, makes it hard to
    // correct the texture on the sides and front of the fillet."
    //
    // WHAT WAS HAPPENING.  Mode 3 is SEL_MASK_FACE with neither the VERTEX nor
    // the OBJECT bit, so a patch hit was rejected TWICE: ScanList's control-point
    // arm skips it (`if ( !wantVert ) continue;`, :338-341) and this tail fell to
    // the `r.valid = false` below.  ClickSelect's miss arm then CLEARS the
    // selection, so clicking a fillet in face mode did not just fail to select
    // it — it deselected whatever was selected, which is why retexturing the
    // fillet's sides and front was impossible.
    //
    // WHY OBJECT GRANULARITY IS THE CORRECT SEMANTIC AND NOT A FALLBACK.  A patch
    // HAS NO FACES.  Its symbiont brush is a bounding box (AddBrushForPatch), so
    // "the face of a patch you clicked" is a box side and would texture the wrong
    // thing.  The patch itself is the finest thing there is to name, so in a mode
    // whose whole meaning is "pick the surface under the cursor" the patch IS the
    // surface — and that is the same reading round AK made for mode 5, where it
    // stopped the control-point scan from swallowing the click so the patch could
    // resolve as an object and reach Brush_SetTexture (kiwi_pick.cpp:383-384).
    // This is that ruling applied to the one mode it did not reach.
    //
    // SCOPED TO PATCHES.  A prefab/model hit still returns invalid in face mode:
    // spec §2 has no editing inside prefab instances, and unlike a patch a prefab
    // instance is not a single texturable surface.
    else if ( hb->patch )
        r.item = Sel_MakeObject( hb );
    else
        r.valid = false;                       // face-only mask, no resolvable face

    return r;
}

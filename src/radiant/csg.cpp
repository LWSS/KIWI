#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// cod3src\radiant\csg.cpp
// Brush CSG: hollow, merge, and the AutoCaulk visibility test.
//   CSG_MakeHollow 0x47D3C0, Brush_MergeList 0x47D600, CSG_Merge 0x47DA40,
//   Brush_GetFaceFlags 0x47DC30, Brush_IsFixedOrPatch 0x47DD20,
//   Brush_FaceFlags_Check80 0x47DDB0, Brush_IsFiltered 0x47DDE0,
//   Brush_IsCulled 0x47DE10, CSG_FaceVisible 0x47DE60,
//   Brush_AutoCaulkFace 0x47E080, Brush_AutoCaulk 0x47E0F0.
//
// List model (CoD4Radiant, NOT GtkRadiant): selected_brushes / active_brushes /
// filtered_brushes are EMBEDDED 56-byte selbrush_t sentinel NODES, iterated
// `for (b = selected_brushes.next; b != &selected_brushes; b = b->next)`.
// selbrush_t.def (+0x14) is the 88-byte brush_t DEFINITION holding the geometry;
// many instances may share one def (def->refCount).  Geometry comes from
// def->faceCount (+0x40) / def->faces (+0x44), a FLAT array of stride 232.

#include "stdafx.h"
#include "winding.h"
#include <universal/assertive.h>   // iassert (USE_ASSERTS always on; same handler as Assert)
#include "kiwi_ported.h"           // KIWI: shared engine/editor externs
#include "kiwi_csg.h"              // KIWI: Merge's failure hint
#include <math.h>
#include <vector>

// ─── externs from brush.cpp ──────────────────────────────────────────────────
// Splits brush b with face f; front/back are new brushes (NULL if no piece).
extern void     Brush_SplitBrushByFace( brush_t *b, face_t *f,
                                         brush_t **front, brush_t **back );    // 0x471960
// Allocates a new face slot in dest's face array and copies src into it.
// Returns a pointer to the newly allocated face entry.
// IDA signature: char* __cdecl Face_Alloc(int brush, _DWORD *face)
extern face_t  *Face_Alloc( brush_t *dest, face_t *src );                    // 0x471500
// Updates brush colour based on entity class.
extern unsigned int Brush_ApplyEntityColor( brush_t *b );                      // brush.cpp 0x475110
// Applies a face material to the face in brush; used by AutoCaulk.
extern void     Brush_SetFaceTexdefSize( const patchMesh_material *material, face_t *f, brush_t *b );   // brush.cpp 0x476740

// CRT free thunk (j__free_0) — matches the eclass.cpp pattern.
static inline void j__free_0( void *p ) { free( p ); }

// ─── globals ──────────────────────────────────────────────────────────────────
// Active layer name string (default "000_Global").
extern char     g_activeLayer_string[];                                         // 0x73bf78

// g_windingAlloc (winding.cpp, 0x24CE4FC) — live winding malloc/free count.

// ─────────────────────────────────────────────────────────────────────────────
//  Face by native array index; face_t contains pointers and grows on x64.
// ─────────────────────────────────────────────────────────────────────────────
static inline face_t *BrushFaceAt( brush_t *def, unsigned int idx )
{
    return &def->faces[idx];
}

// ─────────────────────────────────────────────────────────────────────────────
// Brush_GetFaceFlags (0x47DC30).  Accumulates the current-edit-layer
// material flags over every face of the brush geometry.  Any output pointer may be
// NULL.  *anyInUse = OR of qtexture_s::in_use, *allInUse = AND (init -1),
// *allFilter = AND of color_or_surfacetype_filter (init -1).  The binary also
// accumulates the OR of the filter word but never returns it.
// Asserts csg.cpp:302 "def" and csg.cpp:303 "!def->patch".
// ─────────────────────────────────────────────────────────────────────────────
void Brush_GetFaceFlags( brush_t *def,
                         int *out_anyInUse,
                         int *out_allInUse,
                         int *out_allFilter )
{
    iassert( def );
    iassert( !def->patch );

    int anyInUse  = 0;
    int allInUse  = -1;
    int anyFilter = 0;   // accumulated but not returned
    int allFilter = -1;  // returned in *out_allFilter

    int layer = g_qeglobals.current_edit_layer;

    for ( unsigned int fi = 0; fi < (unsigned int)def->faceCount; fi++ )
    {
        face_t *f = BrushFaceAt( def, fi );
        // &face->mtldef[layer] (mtldef starts at face_t+0x24, MaterialDef is 36 B).
        MaterialDef *md = &f->mtldef[layer];

        qtexture_s *tex = MaterialDef_GetLayeredMaterial( md );
        // qtexture_s::in_use is at +0x20, color_or_surfacetype_filter at +0x1c.
        int inUse = tex ? tex->in_use : 0;
        anyInUse |= inUse;
        allInUse &= inUse;

        tex = MaterialDef_GetLayeredMaterial( md );
        int filt = tex ? tex->color_or_surfacetype_filter : 0;
        anyFilter |= filt;
        allFilter &= filt;
    }

    if ( out_anyInUse )  *out_anyInUse  = anyInUse;
    if ( out_allInUse )  *out_allInUse  = allInUse;
    if ( out_allFilter ) *out_allFilter = allFilter;
}

// ─────────────────────────────────────────────────────────────────────────────
// Brush_IsFixedOrPatch (0x47DD20).  True when the brush must be skipped in
// CSG ops: def->owner->eclass->fixedsize or def->patch != NULL.
// ─────────────────────────────────────────────────────────────────────────────
bool Brush_IsFixedOrPatch( brush_t *def )
{
    iassert( def );
    iassert( def->owner );
    iassert( def->owner->eclass );
    return def->owner->eclass->fixedsize || ( def->patch != nullptr );
}

// ─────────────────────────────────────────────────────────────────────────────
// CSG_FaceVisible (0x47DE60).  Recursive winding-clip visibility test used by
// AutoCaulk.  Walks the selected-brush display list from `brushNode`; for each
// eligible brush it clips `w` face by face, keeping the BACK piece (the part inside
// that convex brush) and recursing on the FRONT piece into the rest of the list.
//   returns 1 = the winding survived  -> the face is VISIBLE
//   returns 0 = the winding was fully accounted for -> the face is HIDDEN (caulk it)
// `w` is CONSUMED (freed) on every path.  Call with faceIdx = 0 and
// brushNode = selected_brushes.next; `srcDef` is skipped (it owns the face).
// The per-brush face-count LIMIT comes from the selbrush_t NODE (+0x18), while the
// face DATA comes from node->def; assert csg.cpp:407.
// plane.dist is read from a DOUBLE slot at plane+0x10, but Plane_FromPoints computes
// it as a float and merely widens it for storage, so the port's float dist is exactly
// the binary's double.  (plane_t = {vec3 normal; int type; float dist; float unk} = 24 B,
// so face.plane.dist is at face+0xC0+0x10 = 0xD0 — where the binary reads it.)
// ─────────────────────────────────────────────────────────────────────────────
bool CSG_FaceVisible( winding_t *w, float *faceNormal, brush_t *srcDef,
                      selbrush_t *b, unsigned int faceIndex )
{
    // The caller's faceIndex is ignored; the walk always starts at face 0.
    faceIndex = 0;

    if ( !w )
        return false;

    while ( true )
    {
        // Next eligible node: not srcDef, not fixed/patch, and passing the filter
        // gate !(anyInUse & 0x20000000) && (allInUse & 1).
        while ( b != &selected_brushes )
        {
            brush_t *def = b->def;

            if ( def != srcDef && !Brush_IsFixedOrPatch( def ) )
            {
                int anyInUse = 0, allInUse = 0, allFilter = 0;
                Brush_GetFaceFlags( def, &anyInUse, &allInUse, &allFilter );
                if ( ( anyInUse & 0x20000000 ) == 0 && ( allInUse & 1 ) != 0 )
                    break;  // found eligible
            }
            b = b->next;
        }

        if ( b == &selected_brushes )
        {
            // Sentinel: nothing left to clip against, the winding survived → VISIBLE.
            --g_windingAlloc;
            free( w );
            return true;
        }

        brush_t *def = b->def;

        // The bound is the NODE's faceCount (+0x18), not def->faceCount.  faceIndex is
        // unsigned, so the binary's `jb` reduces the >= 0 clause to the upper-bound test.
        iassert( faceIndex >= 0 && faceIndex < b->faceCount );   // csg.cpp:407

        face_t *clipFace = BrushFaceAt( def, faceIndex );
        plane_t *clipPlane = &clipFace->plane;

        winding_t *front = nullptr, *back = nullptr;
        int sideCode = Winding_Clip( w, clipPlane->normal, (double)clipPlane->dist,
                                     0.1f, &front, &back );

        --g_windingAlloc;
        free( w );
        w = back;  // back = piece on the clip plane's back side

        selbrush_t *nextNode = b->next;
        if ( CSG_FaceVisible( front, faceNormal, srcDef, nextNode, 0 ) )
        {
            // The front piece is visible through the rest of the list → so is the face.
            if ( w )
            {
                --g_windingAlloc;
                free( w );
            }
            return true;
        }

        // The dot test runs ONLY when Winding_Clip returned SIDE_ON (2 = winding fully
        // coplanar with the clip plane).  For 0/1/3 the walk advances to the next FACE of
        // the same brush; only a coplanar SAME-FACING clip skips to the next brush.
        if ( sideCode == 2 )
        {
            float dot = clipPlane->normal[0] * faceNormal[0]
                      + clipPlane->normal[1] * faceNormal[1]
                      + clipPlane->normal[2] * faceNormal[2];
            if ( dot > 0.0f )
            {
                // Coplanar same-facing → next brush, face index reset.
                b = b->next;
                faceIndex = 0;
                if ( !w )
                    return false;
                continue;
            }
        }

        ++faceIndex;

        if ( !w )
            return false;

        if ( faceIndex >= (unsigned int)b->faceCount )
        {
            // Every face of this brush clipped it and it is still inside → HIDDEN.
            --g_windingAlloc;
            free( w );
            return false;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// CSG_MakeHollow (0x47D3C0).  For each selected brush, inset every face by one grid
// unit along its normal, split the brush on the inset plane, DISCARD the back piece
// (the new inner-shell fragment) and keep the front piece (the wall slab); then free
// the original.  gridScale = grid_sizes[g_qeglobals.d_gridsize].
// The iterator saves p_prev = &node->next->prev BEFORE any free; in a doubly-linked
// list with prev at offset 0 that address IS node->next.
// ─────────────────────────────────────────────────────────────────────────────
void CSG_MakeHollow()
{
    selbrush_t *b = selected_brushes.next;

    while ( b != &selected_brushes )
    {
        brush_t  *def   = b->def;     // selbrush_t.def → 88-byte geometry
        entity_s *owner = b->owner;   // selbrush_t.owner

        // Saved before any free — see the header note.
        selbrush_t *nextNode = b->next;

        iassert( b->owner->def == b->def->owner );   // csg.cpp:34

        // Skip fixed-size entities, patches and xx5-flagged brushes.
        patch_t *nodePatch = b->patch;   // selbrush_t.patch (instance)
        int      nodeXx5   = b->xx5;     // selbrush_t.xx5

        if ( !def->owner->eclass->fixedsize && !nodePatch && !nodeXx5 )
        {
            float gridScale = grid_sizes[g_qeglobals.d_gridsize];
            unsigned int faceCount = (unsigned int)def->faceCount;

            for ( unsigned int fi = 0; fi < faceCount; fi++ )
            {
                face_t *srcFace = BrushFaceAt( def, fi );

                face_t face;
                memcpy( &face, srcFace, sizeof(face_t) );

                // move = plane.normal * gridScale (planepts0[48..50] == plane.normal).
                float moveX = srcFace->plane.normal[0] * gridScale;
                float moveY = srcFace->plane.normal[1] * gridScale;
                float moveZ = srcFace->plane.normal[2] * gridScale;

                // Subtract it from all 3 planepts of the COPIED face.
                for ( int pi = 0; pi < 3; pi++ )
                {
                    face.planepts[pi][0] -= moveX;
                    face.planepts[pi][1] -= moveY;
                    face.planepts[pi][2] -= moveZ;
                }

                brush_t *front = nullptr, *back = nullptr;
                Brush_SplitBrushByFace( def, &face, &front, &back );

                if ( back )
                {
                    Entity_UnlinkBrush( back );   // entity.cpp:1208
                    Brush_Free_R( back );
                }

                if ( front )
                {
                    // Brush_AddToList allocates the selbrush_t INSTANCE for the new def
                    // and links it to the entity; the DISPLAY-list links (next/prev,
                    // not the def's onext/oprev) must still be clear.
                    selbrush_t *added = Brush_AddToList( front, owner );
                    if ( added->next || added->prev )
                    {
                        Com_Error( ERR_FATAL, "Brush_AddToList: already linked" );
                    }
                    Brush_AddToList2( added );
                }
            }  // for each face

            Brush_Free( b );
        }

        b = nextNode;
    }

    // The binary sets this at the END of the whole list loop, unconditionally.
    g_nUpdateBits = -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// Brush_MergeList (0x47D600).  Try to merge a singly-linked list of selbrush_t nodes
// (threaded via .next, NULL-terminated — CSG_Merge builds it by reusing each instance's
// own link, NOT the circular display list) into one new convex brush; NULL if the
// result would be non-convex or carry duplicate planes.  The input list is not freed.
// KIWI: the two phases below are the RETAIL algorithm, replaced by KmConvexUnion.
//   Phase 1 — for every pair of OUTER faces (a face is INNER when another brush in the
//     list has a flipped-equal plane), reject if Winding_PlanesConcave.
//   Phase 2 — Face_Alloc every outer face whose plane is not already in the new brush.
//   Then link the new def to the entity and build its windings.
// Plane_Equal(p2, p1, 1) negates its FIRST arg, so it means "p1 flipped == p2"
// (touching); Plane_Equal(p1, p2, 0) is the same-side test.
// The binary reads the phase-2 loop bound from the selbrush_t NODE's faceCount (+0x18)
// but the face DATA from node->def; the two mirror each other in the editor, so this
// port uses def->faceCount throughout.
// ─────────────────────────────────────────────────────────────────────────────
// KIWI (2026-10-14, user: "the boolean merge of brushes not allowing a merge when the
// shape is seemingly easy to merge ... kisak_trash"): the ported test calls a face
// interior only when another brush carries its EXACT flipped plane, so brushes that
// OVERLAP always read as a concave hull - kisak_trash had 445 overlapping wall-piece
// pairs with a box-shaped union that were refused.  The union is now tested directly:
// the hull planes are the brush faces with every input vertex behind them, the hull
// must stay inside the inputs' bounds, and the hull minus every input must leave
// nothing thicker than KMERGE_GAP.  Coplanar outer faces keep one face, a non-caulk one
// first, then the largest.
namespace
{
    const double KMERGE_EPS     = 0.02;      // vertex-behind-plane slack (DIST_EPSILON)
    const double KMERGE_SAME    = 0.99999;   // normal dot for "one plane"
    const double KMERGE_GAP     = 0.05;      // leftover pieces thinner than this are noise
    const double KMERGE_MIN_VOL = 0.001;     // cubic units: below it a piece is noise
    const double KMERGE_COINCIDE   = 0.999999;   // planes this close are one face
    const double KMERGE_COINCIDE_D = 0.001;
    const int    KMERGE_PIECES  = 512;       // coverage work cap

    struct kmPlane_t { double n[3]; double d; };
    typedef std::vector<kmPlane_t> kmPoly_t;

    double KmDot( const double *a, const double *b ) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }

    bool KmPlaneOf( const face_t *f, kmPlane_t *out )
    {
        // Face_MakePlane's convention, in double: n = (p0-p1) x (p2-p1), d = n.p1.
        double a[3], b[3];
        for ( int k = 0; k < 3; ++k )
        {
            a[k] = (double)f->planepts[0][k] - (double)f->planepts[1][k];
            b[k] = (double)f->planepts[2][k] - (double)f->planepts[1][k];
        }
        double n[3] = { a[1]*b[2] - a[2]*b[1], a[2]*b[0] - a[0]*b[2], a[0]*b[1] - a[1]*b[0] };
        const double l = sqrt( KmDot( n, n ) );
        if ( !( l > 1.0e-12 ) )
            return false;
        for ( int k = 0; k < 3; ++k )
            out->n[k] = n[k] / l;
        double p1[3] = { f->planepts[1][0], f->planepts[1][1], f->planepts[1][2] };
        out->d = KmDot( out->n, p1 );
        return true;
    }

    // One face of a convex polytope: a big square on plane i clipped by the rest.
    void KmFace( const kmPoly_t &poly, size_t i, std::vector<double> *w )
    {
        const kmPlane_t &p = poly[i];
        int ax = 0;
        for ( int k = 1; k < 3; ++k )
            if ( fabs( p.n[k] ) > fabs( p.n[ax] ) ) ax = k;
        double up[3] = { 0.0, 0.0, 0.0 };
        up[( ax == 2 ) ? 0 : 2] = 1.0;
        const double du = KmDot( up, p.n );
        double v[3], r[3];
        for ( int k = 0; k < 3; ++k ) v[k] = up[k] - p.n[k] * du;
        const double lv = sqrt( KmDot( v, v ) );
        for ( int k = 0; k < 3; ++k ) v[k] /= lv;
        r[0] = v[1]*p.n[2] - v[2]*p.n[1];
        r[1] = v[2]*p.n[0] - v[0]*p.n[2];
        r[2] = v[0]*p.n[1] - v[1]*p.n[0];
        const double S = 262144.0;
        static const double SX[4] = { -1, 1, 1, -1 }, SY[4] = { 1, 1, -1, -1 };
        w->clear();
        for ( int c = 0; c < 4; ++c )
            for ( int k = 0; k < 3; ++k )
                w->push_back( p.n[k] * p.d + ( r[k] * SX[c] + v[k] * SY[c] ) * S );

        std::vector<double> out;
        for ( size_t j = 0; j < poly.size() && !w->empty(); ++j )
        {
            if ( j == i )
                continue;
            const kmPlane_t &q = poly[j];
            // A coincident plane is the same face: the first copy owns it.
            if ( KmDot( p.n, q.n ) > KMERGE_COINCIDE && fabs( p.d - q.d ) < KMERGE_COINCIDE_D )
            {
                if ( j < i )
                    w->clear();
                continue;
            }
            const size_t m = w->size() / 3;
            out.clear();
            for ( size_t a = 0; a < m; ++a )
            {
                const double *pa = &(*w)[a * 3];
                const double *pb = &(*w)[( ( a + 1 ) % m ) * 3];
                const double da = KmDot( q.n, pa ) - q.d;
                const double db = KmDot( q.n, pb ) - q.d;
                if ( da <= 0.0 )
                    out.insert( out.end(), pa, pa + 3 );
                if ( ( da < 0.0 && db > 0.0 ) || ( da > 0.0 && db < 0.0 ) )
                {
                    const double t = da / ( da - db );
                    for ( int k = 0; k < 3; ++k )
                        out.push_back( pa[k] + ( pb[k] - pa[k] ) * t );
                }
            }
            w->swap( out );
            if ( w->size() < 9 )
                w->clear();
        }
    }

    // Volume and surface area; false = empty or unbounded (a face reaches the seed square).
    bool KmMeasure( const kmPoly_t &poly, double *outVol, double *outArea )
    {
        std::vector< std::vector<double> > faces( poly.size() );
        double c[3] = { 0.0, 0.0, 0.0 };
        size_t nc = 0;
        for ( size_t i = 0; i < poly.size(); ++i )
        {
            KmFace( poly, i, &faces[i] );
            for ( size_t p = 0; p < faces[i].size(); p += 3 )
            {
                for ( int k = 0; k < 3; ++k )
                {
                    if ( fabs( faces[i][p + k] ) > 131072.0 )
                        return false;
                    c[k] += faces[i][p + k];
                }
                ++nc;
            }
        }
        if ( nc < 4 )
            return false;
        for ( int k = 0; k < 3; ++k ) c[k] /= (double)nc;
        double vol = 0.0, area = 0.0;
        int    sides = 0;
        for ( size_t i = 0; i < poly.size(); ++i )
        {
            const std::vector<double> &w = faces[i];
            const size_t m = w.size() / 3;
            if ( m < 3 )
                continue;
            ++sides;
            double s[3] = { 0.0, 0.0, 0.0 };
            for ( size_t a = 1; a + 1 < m; ++a )
            {
                double e1[3], e2[3];
                for ( int k = 0; k < 3; ++k )
                {
                    e1[k] = w[a * 3 + k] - w[k];
                    e2[k] = w[( a + 1 ) * 3 + k] - w[k];
                }
                s[0] += e1[1]*e2[2] - e1[2]*e2[1];
                s[1] += e1[2]*e2[0] - e1[0]*e2[2];
                s[2] += e1[0]*e2[1] - e1[1]*e2[0];
            }
            const double fa = 0.5 * sqrt( KmDot( s, s ) );
            area += fa;
            vol  += fa * ( poly[i].d - KmDot( poly[i].n, c ) ) / 3.0;
        }
        *outVol  = vol;
        *outArea = area;
        return sides >= 4 && vol > 0.0;
    }

    // A piece that matters: a bounded solid, thicker than the gap tolerance.  Flat
    // remnants (three faces, no volume) come from coincident planes and are noise.
    bool KmSolid( const kmPoly_t &poly )
    {
        double v = 0.0, a = 0.0;
        if ( !KmMeasure( poly, &v, &a ) )
            return false;
        return v > KMERGE_MIN_VOL && a > 0.0 && 2.0 * v / a > KMERGE_GAP;
    }

    bool KmIsCaulk( face_t *f )
    {
        MaterialDef *md = &f->mtldef[g_qeglobals.current_edit_layer];
        if ( ( ( md->lyrMtl != 0 ) + ( md->radMtl != 0 ) ) != 1 )
            return false;
        const char *name = (const char *)Materialdef_GetName( md );
        if ( !name )
            return false;
        const char *leaf = name;
        for ( const char *p = name; *p; ++p )
            if ( *p == '/' || *p == '\\' ) leaf = p + 1;
        return _stricmp( leaf, "caulk" ) == 0;
    }

    double KmWindingArea( const winding_t *w )
    {
        if ( !w || w->numpoints < 3 )
            return 0.0;
        double s[3] = { 0.0, 0.0, 0.0 };
        for ( int a = 1; a + 1 < w->numpoints; ++a )
        {
            double e1[3], e2[3];
            for ( int k = 0; k < 3; ++k )
            {
                e1[k] = (double)w->p[a][k] - (double)w->p[0][k];
                e2[k] = (double)w->p[a + 1][k] - (double)w->p[0][k];
            }
            s[0] += e1[1]*e2[2] - e1[2]*e2[1];
            s[1] += e1[2]*e2[0] - e1[0]*e2[2];
            s[2] += e1[0]*e2[1] - e1[1]*e2[0];
        }
        return 0.5 * sqrt( KmDot( s, s ) );
    }

    struct kmPick_t { face_t *face; kmPlane_t plane; bool caulk; double area; };

    // KIWI: why the last KmConvexUnion said no, for CSG_Merge's failure hint (kiwi_csg.h).
    // Only CSG_Merge publishes it; Auto Bool's trial merges fail as a matter of course.
    std::vector<kmPoly_t> s_kmGaps;          // what the merged brush would have to fill
    bool                  s_kmNoHull = false;  // no usable hull: the inputs are the problem

    // The hull faces of a convex union, or false when the union is not one convex solid.
    bool KmConvexUnion( selbrush_t *brushList, std::vector<face_t *> *outFaces )
    {
        s_kmGaps.clear();
        s_kmNoHull = true;                   // until the coverage test below is reached
        std::vector<double> verts;
        double mins[3] = { 1e30, 1e30, 1e30 }, maxs[3] = { -1e30, -1e30, -1e30 };
        std::vector<kmPoly_t> inputs;
        for ( selbrush_t *n = brushList; n; n = n->next )
        {
            brush_t *def = n->def;
            if ( !def || !def->faces || def->faceCount < 4 )
                return false;
            kmPoly_t poly;
            for ( int f = 0; f < def->faceCount; ++f )
            {
                kmPlane_t p;
                if ( !KmPlaneOf( &def->faces[f], &p ) )
                    return false;
                poly.push_back( p );
                const winding_t *w = def->faces[f].w;
                for ( int i = 0; w && i < w->numpoints; ++i )
                    for ( int k = 0; k < 3; ++k )
                    {
                        const double x = w->p[i][k];
                        verts.push_back( x );
                        if ( x < mins[k] ) mins[k] = x;
                        if ( x > maxs[k] ) maxs[k] = x;
                    }
            }
            inputs.push_back( poly );
        }
        if ( verts.empty() )
            return false;

        std::vector<kmPick_t> picks;
        for ( selbrush_t *n = brushList; n; n = n->next )
            for ( int f = 0; f < n->def->faceCount; ++f )
            {
                face_t *face = &n->def->faces[f];
                if ( !face->w )
                    continue;
                kmPlane_t p;
                KmPlaneOf( face, &p );
                bool behind = true;
                for ( size_t v = 0; v < verts.size() && behind; v += 3 )
                    behind = KmDot( p.n, &verts[v] ) - p.d <= KMERGE_EPS;
                if ( !behind )
                    continue;
                const kmPick_t cand = { face, p, KmIsCaulk( face ), KmWindingArea( face->w ) };
                bool merged = false;
                for ( size_t k = 0; k < picks.size() && !merged; ++k )
                {
                    kmPick_t &o = picks[k];
                    if ( KmDot( o.plane.n, p.n ) < KMERGE_SAME || fabs( o.plane.d - p.d ) > KMERGE_EPS )
                        continue;
                    merged = true;
                    if ( ( o.caulk && !cand.caulk ) || ( o.caulk == cand.caulk && cand.area > o.area ) )
                        o = cand;
                }
                if ( !merged )
                    picks.push_back( cand );
            }
        if ( picks.size() < 4 )
            return false;

        kmPoly_t hull;
        for ( size_t k = 0; k < picks.size(); ++k )
            hull.push_back( picks[k].plane );

        // The hull may not reach past the inputs: a missing hull facet leaves it open.
        {
            std::vector<double> w;
            for ( size_t i = 0; i < hull.size(); ++i )
            {
                KmFace( hull, i, &w );
                for ( size_t p = 0; p < w.size(); p += 3 )
                    for ( int k = 0; k < 3; ++k )
                        if ( w[p + k] < mins[k] - KMERGE_GAP || w[p + k] > maxs[k] + KMERGE_GAP )
                            return false;
            }
        }

        // Coverage: carve every input out of the hull; nothing solid may remain.
        s_kmNoHull = false;
        std::vector<kmPoly_t> pieces( 1, hull ), next;
        for ( size_t b = 0; b < inputs.size() && !pieces.empty(); ++b )
        {
            next.clear();
            for ( size_t pc = 0; pc < pieces.size(); ++pc )
            {
                kmPoly_t inside = pieces[pc];
                for ( size_t f = 0; f < inputs[b].size(); ++f )
                {
                    kmPlane_t flip = inputs[b][f];
                    for ( int k = 0; k < 3; ++k ) flip.n[k] = -flip.n[k];
                    flip.d = -flip.d;
                    kmPoly_t front = inside;
                    front.push_back( flip );
                    if ( KmSolid( front ) )
                        next.push_back( front );
                    inside.push_back( inputs[b][f] );
                    if ( !KmSolid( inside ) )
                        break;
                }
                if ( (int)next.size() > KMERGE_PIECES )
                {
                    s_kmNoHull = true;           // KIWI: half-carved pieces would mislead
                    return false;
                }
            }
            pieces.swap( next );
        }
        if ( !pieces.empty() )
        {
            s_kmGaps.swap( pieces );             // KIWI: the gaps, for the failure hint
            return false;
        }

        outFaces->clear();
        for ( size_t k = 0; k < picks.size(); ++k )
            outFaces->push_back( picks[k].face );
        return true;
    }
}

selbrush_t *Brush_MergeList( selbrush_t *brushList )
{
    if ( !brushList )
        return nullptr;

    // KIWI: the convex-union test above replaces the ported phase 1 / phase 2 picks.
    std::vector<face_t *> hullFaces;
    if ( !KmConvexUnion( brushList, &hullFaces ) )
        return nullptr;
    {
        brush_t *newBrush = (brush_t *)operator new( sizeof(brush_t) );
        memset( newBrush, 0, sizeof(brush_t) );
        for ( size_t k = 0; k < hullFaces.size(); ++k )
            Face_Alloc( newBrush, hullFaces[k] );

        extern void Brush_SetLayerString( brush_t *b, const char *str );   // brush.cpp 0x4758A0
        Brush_SetLayerString( newBrush, g_activeLayer_string );
        entity_s *listOwner = brushList->owner;
        Entity_LinkBrush( newBrush, (entity_s *)listOwner->def );
        iassert( newBrush->owner == brushList->owner->def );   // csg.cpp:190
        selbrush_t *result = Brush_AddToList( newBrush, listOwner );
        Brush_BuildWindings( newBrush, 0 );

        // A kept plane that only grazes the hull along an edge has no winding.
        bool dropped = false;
        for ( int f = newBrush->faceCount - 1; f >= 0; --f )
            if ( !newBrush->faces[f].w && newBrush->faceCount > 4 )
            {
                Brush_RemoveFace( newBrush, (unsigned int)f );
                dropped = true;
            }
        if ( dropped )
            Brush_BuildWindings( newBrush, 0 );
        return result;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// CSG_Merge (0x47DA40).  Merge every selected brush into one: validate (>=2 selected,
// none fixed-size or patch, all owned by the same entity), unlink them all from
// selected_brushes into a NULL-terminated merge list built from their own .next links,
// call Brush_MergeList, then either free the originals and select the result, or
// re-add the originals on failure.
// ─────────────────────────────────────────────────────────────────────────────
int CSG_Merge()
{
    Sys_Printf( "Merging...\n" );
    KiwiCsg_FailHintClear();   // KIWI: a new operation drops the last failure's hint

    selbrush_t *sel = selected_brushes.next;

    if ( sel == &selected_brushes )
        return Sys_Printf( "No brushes selected.\n" );

    if ( sel->next == &selected_brushes )
        return Sys_Printf( "At least two brushes have to be selected.\n" );

    // Ownership invariant (both entity_s_def*).
    iassert( selected_brushes.next->owner->def == selected_brushes.next->def->owner );   // csg.cpp:222

    void *ownerDef0 = sel->owner->def;

    for ( selbrush_t *b = sel; b != &selected_brushes; b = b->next )
    {
        iassert( b->owner->def == b->def->owner );
        entity_s_def *bOwnerDef = (entity_s_def *)b->owner->def;
        eclass_t *ec = bOwnerDef->eclass;
        if ( *( int * )&ec->fixedsize )
            return Sys_Printf( "Cannot add fixed size entities.\n" );

        if ( b->patch )   // selbrush_t.patch @0x20 — the INSTANCE patch, NOT def->patch@0x50
            return Sys_Printf( "Cannot add patches.\n" );

        if ( bOwnerDef != ownerDef0 )
            return Sys_Printf( "Cannot add brushes from different entities.\n" );
    }

    // Remove each selected node from the display list and push it onto a NULL-terminated
    // merge list, reusing the instance's own next/prev links (captured before removal).
    selbrush_t *mergeList = nullptr;
    for ( selbrush_t *b = sel; b != &selected_brushes; )
    {
        selbrush_t *nextB = b->next;   // capture before RemoveFromList clears the links
        Brush_RemoveFromList( b );
        b->next = mergeList;
        b->prev = nullptr;
        mergeList = b;
        b = nextB;
    }

    selbrush_t *merged = Brush_MergeList( mergeList );

    if ( merged )
    {
        for ( selbrush_t *cur = mergeList; cur; )
        {
            selbrush_t *next = cur->next;
            cur->next = nullptr;
            cur->prev = nullptr;
            Brush_Free( cur );
            cur = next;
        }
        if ( merged->next || merged->prev )   // the display-list links, not onext/oprev
            Com_Error( ERR_FATAL, "Brush_AddToList: already linked" );
        Brush_AddToList2( merged );
        int r = Sys_Printf( "done.\n" );
        g_nUpdateBits = -1;
        return r;
    }
    else
    {
        // KIWI: show where - the gaps the merged brush would have to fill, or the inputs.
        bool hinted = false;
        if ( !s_kmNoHull )
            for ( size_t g = 0; g < s_kmGaps.size(); ++g )
            {
                std::vector<double> planes;
                for ( size_t k = 0; k < s_kmGaps[g].size(); ++k )
                {
                    const kmPlane_t &p = s_kmGaps[g][k];
                    planes.push_back( p.n[0] );
                    planes.push_back( p.n[1] );
                    planes.push_back( p.n[2] );
                    planes.push_back( p.d );
                }
                if ( !planes.empty() && KiwiCsg_FailHintAddSolid( &planes[0], (int)( planes.size() / 4 ) ) )
                    hinted = true;
            }
        if ( !hinted )
            for ( selbrush_t *cur = mergeList; cur; cur = cur->next )
                KiwiCsg_FailHintAddBrush( cur->def );
        s_kmGaps.clear();

        for ( selbrush_t *cur = mergeList; cur; )
        {
            selbrush_t *next = cur->next;
            cur->next = nullptr;
            cur->prev = nullptr;
            Brush_AddToList2( cur );
            cur = next;
        }
        return Sys_Printf( "Cannot add a set of brushes with a concave hull%s.\n",
                           hinted ? " (the gaps it would have to fill are shown in red)" : "" );
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Brush_AutoCaulkFace (0x47E080).  Caulk face `face` of `brush` if CSG_FaceVisible
// says it is hidden by the surrounding brushes.  Returns true on the two caulking
// paths (no winding, or hidden) and false for a visible face — Brush_AutoCaulk counts
// the true returns, so an unconditional `return true` would inflate the count.
// The binary reaches the caulk arm through `&face->w->numpoints`, which is NULL when
// face->w is NULL, i.e. a face with no winding is treated as hidden.
// ─────────────────────────────────────────────────────────────────────────────
bool Brush_AutoCaulkFace( brush_t *brush, face_t *face )
{
    winding_t *w = face->w;
    if ( !w )
    {
        patchMesh_material mat{};
        SetMaterial( "caulk", &mat );
        Brush_SetFaceTexdefSize( &mat, face, brush );   // the {lyrMtl,radMtl} pair by address
        return true;
    }

    winding_t *wClone = Winding_Clone( w );

    // CSG_FaceVisible returns non-zero when the face is VISIBLE.
    selbrush_t *firstSelected = selected_brushes.next;
    bool hidden = !CSG_FaceVisible( wClone, face->plane.normal, brush, firstSelected, 0 );

    if ( hidden )
    {
        patchMesh_material mat{};
        SetMaterial( "caulk", &mat );
        Brush_SetFaceTexdefSize( &mat, face, brush );
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Brush_AutoCaulk (0x47E0F0).  For every non-fixed/non-patch selected brush whose
// face flags lack 0x80, caulk each face whose current-layer material isn't already
// "caulk" and is hidden by its neighbours; report the count.  The per-face name
// compare reads radMtl->name (qtexture_s+4).  UI command only
// (ON_COMMAND 33220 / OnSelectionAutoCaulk), off the map-load path.
// ─────────────────────────────────────────────────────────────────────────────
extern int  MaterialDef_08( qtexture_s *mtl );       // 0x431990 (passed as the cb)

void Brush_AutoCaulk()
{
    int count = 0;
    selbrush_t *inst = selected_brushes.next;
    if ( inst != &selected_brushes )
    {
        for ( ;; )
        {
            brush_t *def = inst->def;
            if ( !Brush_IsFixedOrPatch( def ) )
            {
                int flags = 0;
                Brush_GetFaceFlags( def, nullptr, nullptr, &flags );
                if ( ( flags & 0x80 ) == 0 )
                {
                    for ( unsigned int fi = 0; fi < (unsigned int)def->faceCount; ++fi )
                    {
                        face_t      *fp = &def->faces[fi];
                        MaterialDef *md = &fp->mtldef[g_qeglobals.current_edit_layer];
                        dword_181F51C = 128;
                        MaterialDef_02( md, MaterialDef_08 );
                        if ( !dword_181F51C )
                        {
                            // MaterialDef.cpp:85 lives in Materialdef_GetName
                            const char *name = (const char *)Materialdef_GetName( md );
                            if ( _stricmp( name, "caulk" ) )
                            {
                                if ( Brush_AutoCaulkFace( def, fp ) )
                                    ++count;
                            }
                        }
                    }
                }
            }
            if ( inst->next == &selected_brushes )
                break;
            inst = inst->next;
        }
    }

    if ( count == 1 )
        Sys_Printf( "1 face caulked\n" );
    else if ( count )
        Sys_Printf( "%i faces caulked\n", count );
    else
        Sys_Printf( "no faces caulked\n" );
    g_nUpdateBits |= 1u;
}

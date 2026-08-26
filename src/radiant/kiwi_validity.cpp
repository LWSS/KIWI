#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Validity checks and baseline restore over the ported brush and patch cores.

#include "stdafx.h"
#include "qe3.h"

#include "kiwi_selection.h"  // Patch_DimsSane / KIWI_PATCH_MAX_DIM
#include "kiwi_validity.h"
#include "kiwi_vec.h"     // Dot3

#include <math.h>
#include <string.h>

// Ported entry points.
extern void Brush_BuildWindings( brush_t *def, int bFull );   // brush.cpp 0x477AC0
extern void Patch_Rebuild( patchMesh_t *p, char doBounds );   // pmesh.cpp; used by Patch_UpdateSelected_0
extern void MarkMapModified();                                // win_qe3.cpp 0x499BB0

extern winding_t *Brush_MakeFaceWinding( face_t *f, brush_t *def );   // brush.cpp:4712 (0x471260)
extern void       Winding_Free( winding_t *w );                      // winding.cpp:153

namespace
{

    // Triangulated planar area is exact because Brush_BuildWindings emits convex windings.
    float WindingArea( const winding_t *w )
    {
        if ( !w || w->numpoints < 3 )
            return 0.0f;
        float total = 0.0f;
        for ( int i = 1; i + 1 < w->numpoints; ++i )
        {
            const float a[3] = { w->p[i][0]     - w->p[0][0],
                                 w->p[i][1]     - w->p[0][1],
                                 w->p[i][2]     - w->p[0][2] };
            const float b[3] = { w->p[i + 1][0] - w->p[0][0],
                                 w->p[i + 1][1] - w->p[0][1],
                                 w->p[i + 1][2] - w->p[0][2] };
            const float c[3] = { a[1] * b[2] - a[2] * b[1],
                                 a[2] * b[0] - a[0] * b[2],
                                 a[0] * b[1] - a[1] * b[0] };
            total += sqrtf( Dot3( c, c ) ) * 0.5f;
        }
        return total;
    }

    // BuildWindings seeds mins=+131072/maxs=-131072 and expands only from intersections;
    // non-increasing bounds therefore cover empty and inside-out results.
    bool BoundsOk( const brush_t *def, const char **outWhy )
    {
        for ( int a = 0; a < 3; ++a )
        {
            if ( !( def->maxs[a] > def->mins[a] ) )
            {
                if ( outWhy ) *outWhy = "empty bounds";
                return false;
            }
            if ( def->maxs[a] - def->mins[a] > KVALID_MAX_SPAN )
            {
                if ( outWhy ) *outWhy = "bounds exploded";
                return false;
            }
            if ( def->mins[a] < -KVALID_MAX_COORD || def->maxs[a] > KVALID_MAX_COORD )
            {
                if ( outWhy ) *outWhy = "outside the map";
                return false;
            }
        }
        return true;
    }
}

// Baseline
bool KiwiValid_Snapshot( brush_t *def, kiwiBaseBrush_t *out )
{
    if ( !def || !out )
        return false;

    *out = kiwiBaseBrush_t();
    out->def = def;

    // Patch_Rebuild replaces the bounding-brush face array; only the control grid is stable.
    patchMesh_t *pm = def->patch;
    // This also enforces patchMesh_t::ctrl's 16x16 storage bound.
    const bool isPatch = Patch_DimsSane( pm );

    if ( isPatch )
    {
        out->patchW = pm->width;
        out->patchH = pm->height;
        out->ctrl.resize( (size_t)pm->width * pm->height * 3 );
        for ( int col = 0; col < pm->width; ++col )
            for ( int row = 0; row < pm->height; ++row )
            {
                const size_t o = (size_t)( col * pm->height + row ) * 3;
                out->ctrl[o + 0] = pm->ctrl[col][row].xyz[0];
                out->ctrl[o + 1] = pm->ctrl[col][row].xyz[1];
                out->ctrl[o + 2] = pm->ctrl[col][row].xyz[2];
            }
        return true;
    }

    out->faceCount = def->faces ? def->faceCount : 0;
    if ( out->faceCount > 0 )
    {
        out->planepts.resize( (size_t)out->faceCount * 9 );
        for ( int f = 0; f < out->faceCount; ++f )
            memcpy( &out->planepts[(size_t)f * 9], &def->faces[f].planepts[0][0],
                    sizeof( float ) * 9 );
    }
    return true;
}

bool KiwiValid_RestoreOnly( const kiwiBaseBrush_t &base )
{
    brush_t *def = base.def;
    if ( !def )
        return false;

    if ( base.faceCount > 0 )
    {
        // Brush_MoveVertex can split/collapse faces, so a changed count invalidates the copy.
        if ( !def->faces || def->faceCount != base.faceCount )
            return false;
        for ( int f = 0; f < base.faceCount; ++f )
            memcpy( &def->faces[f].planepts[0][0], &base.planepts[(size_t)f * 9],
                    sizeof( float ) * 9 );
    }

    if ( base.patchW > 0 )
    {
        patchMesh_t *pm = def->patch;
        if ( !pm || pm->width != base.patchW || pm->height != base.patchH )
            return false;
        for ( int col = 0; col < base.patchW; ++col )
            for ( int row = 0; row < base.patchH; ++row )
            {
                const size_t o = (size_t)( col * base.patchH + row ) * 3;
                pm->ctrl[col][row].xyz[0] = base.ctrl[o + 0];
                pm->ctrl[col][row].xyz[1] = base.ctrl[o + 1];
                pm->ctrl[col][row].xyz[2] = base.ctrl[o + 2];
            }
    }
    return true;
}

void KiwiValid_Rebuild( brush_t *def )
{
    if ( !def )
        return;
    Brush_BuildWindings( def, 0 );      // bFull 0 avoids legacy power-of-two snapping
    MarkMapModified();
    ++def->version;                     // what invalidates the instance faceVis cache
}

// A snapshot contains either patch controls or brush planepts, never both.
bool KiwiValid_Restore( const kiwiBaseBrush_t &base )
{
    if ( !KiwiValid_RestoreOnly( base ) )
        return false;
    if ( base.patchW > 0 && base.def->patch )
        Patch_Rebuild( base.def->patch, 1 );    // the ported control-point bookkeeping
    else if ( base.faceCount > 0 )
        KiwiValid_Rebuild( base.def );
    return true;
}

// Brush validity gate
bool KiwiValid_CheckBrush( brush_t *def, const char **outWhy )
{
    return KiwiValid_CheckBrushIgnoringFace( def, -1, outWhy );
}

// One body serves the full gate and the duplicate-face removal trial.
bool KiwiValid_CheckBrushIgnoringFace( brush_t *def, int ignoreFace, const char **outWhy )
{
    if ( !def )
    {
        if ( outWhy ) *outWhy = "no brush";
        return false;
    }

    // V1: count after exclusion because a solid needs four retained half-spaces.
    if ( !def->faces )
    {
        if ( outWhy ) *outWhy = "too few faces";
        return false;
    }
    {
        int kept = def->faceCount;
        if ( ignoreFace >= 0 && ignoreFace < def->faceCount )
            --kept;
        if ( kept < 4 )
        {
            if ( outWhy ) *outWhy = "too few faces";
            return false;
        }
    }

    for ( int f = 0; f < def->faceCount; ++f )
    {
        if ( f == ignoreFace )
            continue;
        const face_t *fa = &def->faces[f];

        // V2: Face_MakePlane normalises healthy planes.
        const float n2 = Dot3( fa->plane.normal, fa->plane.normal );
        if ( !( n2 > 0.9f ) || !( n2 < 1.1f ) )
        {
            if ( outWhy ) *outWhy = "degenerate plane";
            return false;
        }

        // V3: the winding left by the other half-spaces.
        const winding_t *w = fa->w;
        if ( !w || w->numpoints < 3 )
        {
            if ( outWhy ) *outWhy = "face collapsed";
            return false;
        }
        if ( w->numpoints > MAX_POINTS_ON_WINDING )
        {
            if ( outWhy ) *outWhy = "winding overflow";
            return false;
        }

        // V4: reject a surviving sliver.
        if ( WindingArea( w ) < KVALID_MIN_FACE_AREA )
        {
            if ( outWhy ) *outWhy = "zero-area face";
            return false;
        }
    }

    // V5/V6: pairwise plane relations.
    for ( int i = 0; i < def->faceCount; ++i )
    {
        if ( i == ignoreFace )
            continue;
        const plane_t &pi = def->faces[i].plane;
        for ( int j = i + 1; j < def->faceCount; ++j )
        {
            if ( j == ignoreFace )
                continue;
            const plane_t &pj = def->faces[j].plane;
            const float d = Dot3( pi.normal, pj.normal );

            if ( d > KVALID_PLANE_DOT
              && fabsf( pi.dist - pj.dist ) < KVALID_PLANE_DIST )
            {
                if ( outWhy ) *outWhy = "duplicate plane";
                return false;
            }
            if ( d < -KVALID_PLANE_DOT
              && ( pi.dist + pj.dist ) < KVALID_MIN_THICKNESS )
            {
                if ( outWhy ) *outWhy = "planes crossed";
                return false;
            }
        }
    }

    // V7: rebuilt bounds.
    return BoundsOk( def, outWhy );
}

bool KiwiValid_CheckBounds( brush_t *def, const char **outWhy )
{
    if ( !def )
    {
        if ( outWhy ) *outWhy = "no brush";
        return false;
    }
    // Patches are control-grid geometry; only their rebuilt bounds are meaningful here.
    return BoundsOk( def, outWhy );
}

// Closure probe: an unbounded face reaches a temporary box outside the brush bounds.
// Brush_MakeFaceWinding (brush.cpp 0x471260) reads its seed box from def, so the
// bounds are swapped and restored; it returns caller-owned windings without changing face->w.
bool KiwiValid_BrushCloses( brush_t *def, const char **outWhy )
{
    if ( !def || !def->faces || def->faceCount < 4 )
    {
        if ( outWhy ) *outWhy = "too few faces";
        return false;
    }
    if ( !BoundsOk( def, outWhy ) )
        return false;               // a box we cannot trust cannot seed a probe box

    float saveMins[3], saveMaxs[3];
    float boxMin[3],   boxMax[3];
    for ( int a = 0; a < 3; ++a )
    {
        saveMins[a] = def->mins[a];
        saveMaxs[a] = def->maxs[a];
        def->mins[a] = saveMins[a] - KVALID_CLOSE_MARGIN;
        def->maxs[a] = saveMaxs[a] + KVALID_CLOSE_MARGIN;
        // The ported builder expands def bounds by one additional world unit.
        boxMin[a] = def->mins[a] - 1.0f;
        boxMax[a] = def->maxs[a] + 1.0f;
    }

    bool closed = true;
    for ( int f = 0; f < def->faceCount && closed; ++f )
    {
        // NULL can be the ignored duplicate or a V3 failure already rejected by caller
        // ordering; the surviving duplicate still tests closure.
        winding_t *w = Brush_MakeFaceWinding( &def->faces[f], def );
        if ( !w )
            continue;

        for ( int i = 0; i < w->numpoints && closed; ++i )
            for ( int a = 0; a < 3; ++a )
                if ( w->p[i][a] <= boxMin[a] + KVALID_CLOSE_TOUCH
                  || w->p[i][a] >= boxMax[a] - KVALID_CLOSE_TOUCH )
                {
                    closed = false;
                    break;
                }

        Winding_Free( w );
    }

    for ( int a = 0; a < 3; ++a )
    {
        def->mins[a] = saveMins[a];
        def->maxs[a] = saveMaxs[a];
    }

    if ( !closed && outWhy )
        *outWhy = "the remaining planes do not close the solid";
    return closed;
}

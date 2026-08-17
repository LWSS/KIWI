#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_validity.cpp — RADIANT_UX_DESIGN §19 implementation.  See kiwi_validity.h
// for the exact check list and for why the baseline exists alongside undo.
//
// NEW code over the ported cores: it reads plane/winding data the ported
// Brush_BuildWindings produced and writes back planepts it previously copied out.
// It never reimplements winding or plane math.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"

#include "kiwi_selection.h"  // KIWI-UX (CLEANUP, A-14): Patch_DimsSane / KIWI_PATCH_MAX_DIM
#include "kiwi_validity.h"
#include "kiwi_vec.h"     // KIWI-UX (CLEANUP, A-15): the one spelling of Dot3/Sub3/...

#include <math.h>
#include <string.h>

// ── ported entry points (verified against their definitions) ────────────────
extern void Brush_BuildWindings( brush_t *def, int bFull );   // brush.cpp 0x477AC0
extern void Patch_Rebuild( patchMesh_t *p, char doBounds );   // pmesh.cpp (the control-point
                                                              // rebuild Patch_UpdateSelected_0 uses)
extern void MarkMapModified();                                // win_qe3.cpp 0x499BB0

// ROUND AA, ITEM 4 — V8's two halves.  The Brush_MakeFaceWinding declaration is
// select.cpp:4510's, verbatim, with the definition line added.
extern winding_t *Brush_MakeFaceWinding( face_t *f, brush_t *def );   // brush.cpp:4695 (0x471260)
extern void       Winding_Free( winding_t *w );                      // winding.cpp:153

namespace
{

    // Area of a planar polygon: half the magnitude of the summed edge cross
    // products about p[0].  Exact for any convex winding, which is all
    // Brush_BuildWindings ever produces.
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

    // V7, shared by both public checks.  Brush_BuildWindings seeds the def's box
    // to mins=+131072 / maxs=-131072 and only ever EXPANDS it from the half-space
    // intersection points, so "no intersection points at all" and "inside-out"
    // both surface here as mins >= maxs.
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

// ─── baseline ────────────────────────────────────────────────────────────────
bool KiwiValid_Snapshot( brush_t *def, kiwiBaseBrush_t *out )
{
    if ( !def || !out )
        return false;

    *out = kiwiBaseBrush_t();
    out->def = def;

    // A PATCH's brush def is a BOUNDING BOX around the control grid, and
    // Patch_Rebuild → Brush_RebuildBrush FREES and re-creates that face array from
    // the recomputed bounds every time.  Snapshotting its planepts would therefore
    // capture faces that do not survive the next rebuild, and restoring them would
    // write into freed/replaced memory.  The control grid IS the patch's geometry,
    // so that is the only thing worth a baseline.
    patchMesh_t *pm = def->patch;
    // KIWI-UX (CLEANUP, A-14): the bare 16 was `patchMesh_t::ctrl`'s extent
    // written out by hand here and in kiwi_transform.cpp.  One predicate now,
    // one named bound (KIWI_PATCH_MAX_DIM, kiwi_selection.h).
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
        // The face array must still be the shape we snapshotted.  Brush_MoveVertex
        // is the one core that GROWS faceCount mid-edit (it splits/collapses
        // windings), so a mismatch here means the caller drove a path whose
        // baseline is not a planept copy — it must not silently write 9 floats per
        // face into a differently-shaped array.
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
    Brush_BuildWindings( def, 0 );      // bFull 0 — see kiwi_validity.h
    MarkMapModified();
    ++def->version;                     // what invalidates the instance faceVis cache
}

// Exactly one of the two arms runs: a snapshot is either a patch's control grid
// or a brush's planepts, never both (see KiwiValid_Snapshot).
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

// ─── the gate ────────────────────────────────────────────────────────────────
bool KiwiValid_CheckBrush( brush_t *def, const char **outWhy )
{
    return KiwiValid_CheckBrushIgnoringFace( def, -1, outWhy );
}

// ROUND AA, ITEM 4: the ONE body, with an optional face taken out of the set.
// `ignoreFace` < 0 is the plain gate (KiwiValid_CheckBrush above), so there is
// exactly one copy of V1..V7 in the tree — the duplicate-function drift this
// codebase keeps paying for is what a second, "nearly the same" checker would be.
bool KiwiValid_CheckBrushIgnoringFace( brush_t *def, int ignoreFace, const char **outWhy )
{
    if ( !def )
    {
        if ( outWhy ) *outWhy = "no brush";
        return false;
    }

    // V1 — a solid needs four half-spaces.  Counted AFTER the exclusion: a brush
    // that is only a solid because of the face we are about to drop is not one.
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

        // V2 — Face_MakePlane normalises, so a healthy normal is unit length.
        const float n2 = Dot3( fa->plane.normal, fa->plane.normal );
        if ( !( n2 > 0.9f ) || !( n2 < 1.1f ) )
        {
            if ( outWhy ) *outWhy = "degenerate plane";
            return false;
        }

        // V3 — the winding the other planes left of this face.
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

        // V4 — a sliver that survived instead of disappearing.
        if ( WindingArea( w ) < KVALID_MIN_FACE_AREA )
        {
            if ( outWhy ) *outWhy = "zero-area face";
            return false;
        }
    }

    // V5 / V6 — pairwise plane relations.
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

    // V7 — bounds (the BoundsOk helper, shared with KiwiValid_CheckBounds).
    return BoundsOk( def, outWhy );
}

bool KiwiValid_CheckBounds( brush_t *def, const char **outWhy )
{
    if ( !def )
    {
        if ( outWhy ) *outWhy = "no brush";
        return false;
    }
    // A patch is not plane-defined, so V1..V6 have nothing to say about it — the
    // control grid is free-form by construction.  V7 still applies: Patch_Rebuild
    // writes the tessellated bounds through Brush_RebuildBrush, and an exploded or
    // inverted box is exactly what a runaway control-point drag produces.
    return BoundsOk( def, outWhy );
}

// ─── V8 — "does the solid still close?" (ROUND AA, ITEM 4) ───────────────────
// The whole argument is in kiwi_validity.h's V8 section.  In one line: give the
// brush a probe box a whole KVALID_CLOSE_MARGIN larger than its own bounds, ask
// the PORTED per-face winding builder to rebuild each face against it, and treat
// "a winding point landed on the probe box" as the leak — because a closed
// brush's faces are its real faces and lie inside its own bounds, so they can
// only reach the probe box if some other plane stopped bounding them.
//
// WHY def->[mins,maxs] ARE WRITTEN AND PUT BACK, rather than passing a box in:
// Brush_MakeFaceWinding (brush.cpp:4684) reads its base box straight off the def
// — `Winding_BaseForPlane` over def->[mins,maxs] expanded ±1 — and it is the
// ported spelling of "the polygon these planes leave of this face".  Re-deriving
// that clip loop here with a box parameter would be a second copy of it, so the
// def's box is swapped instead.  It is a pure scratch write: the two vectors are
// saved on entry and restored on EVERY exit path, and nothing else in the def is
// touched (the windings this builds are the caller's-owned copies it frees, not
// face->w, which Brush_MakeFaceWinding never assigns).
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
        // …and Brush_MakeFaceWinding expands what it reads by one more unit, so
        // THIS is the surface a clipped point actually lands on.
        boxMin[a] = def->mins[a] - 1.0f;
        boxMax[a] = def->maxs[a] + 1.0f;
    }

    bool closed = true;
    for ( int f = 0; f < def->faceCount && closed; ++f )
    {
        // NULL is not a leak.  Brush_MakeFaceWinding returns it for a face whose
        // plane duplicates an earlier one ("this face is the duplicate", its own
        // words) and for a plane the others clipped away entirely — the first is
        // exactly the state Match Face's planarize-away hands us and is answered
        // by the surviving twin, the second is V3's business and has already been
        // decided by the time anyone calls this.
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

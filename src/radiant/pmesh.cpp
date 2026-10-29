#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// cod3src\radiant\pmesh.cpp
// ═════════════════════════════════════════════════════════════════════════════
//  PATCH MESH (curve/terrain) subsystem: parse, write, tessellate, edit and draw.
//
//  Round-trip data flow:
//    LOAD  Brush_Parse -> Patch_ParseMesh -> MakeNewPatch + Patch_ParseVert(xN) +
//          AddBrushForPatch (symbiont brush_t, brush->patch = patch).  ParseEntity links
//          the brush into the entity's def-list.
//    SAVE  Brush_Write -> (brush->patch) -> Patch_Write -> Patch_WriteVert(xN).
//  The writer reads ONLY the control grid / materials / params from patchMesh_t, never
//  the subdivided render mesh (curveDef) - so the tessellation is display-only and never
//  affects the round-trip.
//
//  CoD's .map uses `mesh`/`curve` blocks with a per-vertex
//  `v xyz [c rgba] t s0 t0 s1 t1 [f edge]` grammar - NOT GtkRadiant's patchDef2/patchDef3.
// ═════════════════════════════════════════════════════════════════════════════

#include "stdafx.h"
#include "qe3.h"
#include "kiwi_matconvert.h"        // KIWI-UX: solid-red Shift+L patch diagnostic
#include <universal/q_parse.h>
#include <gfx_d3d/r_gfx.h>          // GfxPointVertex, GfxColor, GfxStateBits (patch-fill)
#include <gfx_d3d/r_material.h>     // Material stateBitsEntry/stateBitsTable (patch-fill opaque gate)
#include <gfx_d3d/r_state.h>        // GFXS1_DEPTHWRITE
#include <gfx_d3d/r_rendercmds.h>   // GfxCmdDrawPoints (R_AddPointCmd_W return)
#include <stdlib.h>   // atof, atol, atoi, free
#include <string.h>
#include <stdio.h>    // sprintf
#include "kiwi_floatsnap.h"   // KIWI: noise-only half-grid snap
#include "kiwi_angles.h"      // KIWI: exact sin / cos of degrees
#include "kiwi_ported.h"      // KIWI: shared engine/editor externs

// ─── editor / engine externs (ported elsewhere) ──────────────────────────────
extern void  Error( const char *fmt, ... );

// materialdef.cpp
namespace LayerMat { int GetCurrentLayer( MaterialDef *def ); }

// brush.cpp
extern texdef_sub_t *MaterialDef_FindTexdef( MaterialDef *mtlDef, int layerHandle );   // brush.cpp 0x45D360

// ─── PATCH CREATE + GRID MANIPULATION deps ───────────────────────────────────
// brush.cpp — rebuild a patch symbiont's 6-face bbox brush from new bounds (0x438760).
extern void       Brush_RebuildBrush( brush_t *b, float *mins, float *maxs );

// engine_stubs.cpp (mapparsing.cpp ports) — `contents <names>;` / `toolFlags <names>;`
extern int      Map_ParseContentsLine( const char **text );    // reads a "contents"  line → bits
extern int      Map_ParseToolFlagsLine( const char **text );   // reads a "toolFlags" line → bits
// layers.cpp — optional `<keyword> "value"` (or unquoted) line, else default.
extern void     Map_ParseEntityLayerKey( const char **text, const char *def,
                                          char *out, const char *key );
// engine_stubs.cpp — contents / toolFlags flag-name tables (for the writer).
extern int     *contents_table;
extern int     *toolflags_table;
extern void     MapLoad_ParseBrush_Content( const char *keyword, int (**writer)(void *, const char *, ...),
                                            int value, void *table );
extern void     MapLoad_ParseBrush_Layer( int (**writer)(void *, const char *, ...),
                                          const char *layerStr );   // engine_stubs 0x42fb40

// ─── forward declarations (this file) ────────────────────────────────────────
patchMesh_t *MakeNewPatch();
void         Patch_CalcBounds( float *mins, float *maxs, patchMesh_t *p, bool expand );
brush_t     *AddBrushForPatch( patchMesh_t *p, entity_s *world_ent );
static char  Patch_ParseVert( const char **text, drawVert_t *v );
static int   Patch_CalcVertColors( patchMesh_t *p );
static curvePatchDef_t *Patch_TerrainTexProject( patchMesh_t *p, int layer, float sampleSize );
static void  Patch_ResampleCurveST( patchMesh_t *p, int layer, float sampleSize );

// Patch-inspector dialog refresh hooks (patchdialog.cpp).  PatchDialog_IsOpen() IS the
// binary's `if (CWnd_PatchDialog.m_hWnd)` gate — true only while the modeless
// CPatchInspectorDlg exists, false headless AND false in the no-MFC shell (the ImGui
// patch panel re-gathers, so there is no push to make).
extern bool  PatchDialog_IsOpen();
extern void  g_PatchDialog_GetPatchInfo();

// ════════════════════════════════════════════════════════════════════════════
//  MakeNewPatch  (0x437ac0)
//  Allocates a zeroed patchMesh_t and seeds default materials + white vertices.
// ════════════════════════════════════════════════════════════════════════════
patchMesh_t *MakeNewPatch()
{
    patchMesh_t *p = (patchMesh_t *)operator new( sizeof( patchMesh_t ) );   // 0x504C
    *(float *)&p->size_of_struct_0x504C = 0.0f;
    p->width      = 0;
    p->height     = 0;
    p->contents   = 0;
    p->type       = (PATCH_TYPES)0;
    p->subDivType = 8;
    SetMaterial( "$default",         &p->texture );
    SetMaterial( "lightmap_gray",    &p->lightmap );
    SetMaterial( "smoothing_smooth", &p->smoothing );
    memset( p->ctrl, 0, sizeof( p->ctrl ) );
    for ( int w = 0; w < 16; ++w )
        for ( int h = 0; h < 16; ++h )
            *(unsigned int *)&p->ctrl[w][h].vert_color = 0xFFFFFFFF;   // white
    p->curveDef = nullptr;
    p->pSymbiot = nullptr;
    p->version  = 0;
    p->xx22b    = false;
    memset( p->kiwiLayer, 0, sizeof( p->kiwiLayer ) );   // KIWI: no texture layers
    return p;
}

// ════════════════════════════════════════════════════════════════════════════
//  Patch_CalcBounds  (0x4385e0)
//  Computes the AABB of the control points; optionally expands degenerate axes
//  (< 8 units) to a centred 8-unit span so the symbiont bbox brush is non-empty.
//  The per-vertex VectorMaxValues (a fatal stub in this build) is inlined.
// ════════════════════════════════════════════════════════════════════════════
void Patch_CalcBounds( float *mins, float *maxs, patchMesh_t *p, bool expand )
{
    ++p->version;
    mins[0] = mins[1] = mins[2] =  131072.0f;
    maxs[0] = maxs[1] = maxs[2] = -131072.0f;

    for ( int w = 0; w < p->width; ++w )
    {
        for ( int h = 0; h < p->height; ++h )
        {
            const float *xyz = p->ctrl[w][h].xyz;
            for ( int k = 0; k < 3; ++k )
            {
                if ( xyz[k] < mins[k] ) mins[k] = xyz[k];
                if ( xyz[k] > maxs[k] ) maxs[k] = xyz[k];
            }
        }
    }

    if ( expand )
    {
        for ( int k = 0; k < 3; ++k )
        {
            if ( maxs[k] - mins[k] < 8.0f )
            {
                float c = ( mins[k] + maxs[k] ) * 0.5f - 4.0f;
                mins[k] = c;
                maxs[k] = c + 8.0f;
            }
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  PATCH TESSELLATION (the Bezier subdivision that produces curveDef, the render
//  mesh).  All structs (curveVert_t / curvePatchDef_t / patchMesh_t) are editor-only
//  (NOT shared engine structs), so this is radiant-only.  The chain:
//      Patch_GenericMesh2  control grid → flat curveVert array, then:
//        TERRAIN  → copy verts as-is, compute normals (Curve_ComputeNormals)
//        BEZIER   → Curve_SubdivideSetup (subdivide both axes via de Casteljau)
//                   → Curve_MidpointFixup → Curve_ComputeNormals
//                   → Curve_RemoveDegenerate (drop collapsed rows/cols)
//  The S/T fields are filled but UNUSED by the wireframe path (geometry only); the
//  curveDef-dependent texCoord REFINEMENT (Patch_ResampleCurveST / Patch_TerrainTexProject)
//  affects only texturing, not the wireframe grid.  The de Casteljau midpoints are plain
//  (a+b)*0.5.
// ════════════════════════════════════════════════════════════════════════════

// A fixed 512x512 curveVert scratch grid (512*512*44 bytes, as in the binary); the
// editor is single-threaded and tessellation is non-reentrant.  Row stride is
// CURVE_GRID_DIM verts.
static const int CURVE_GRID_DIM = 512;
static curveVert_t s_curveScratch[CURVE_GRID_DIM][CURVE_GRID_DIM];
// row 0 / row 1 aliases.
#define CURVE_ROW0 ( &s_curveScratch[0][0] )
#define CURVE_ROW1 ( &s_curveScratch[1][0] )

// ─── 0x4a47d0 — normalize `in` into `out`, return original length (0-guarded). ─
static float Curve_NormalizeTo( float *out, const float *in )
{
    float len = (float)sqrt( in[0] * in[0] + in[1] * in[1] + in[2] * in[2] );
    float d   = len;
    if ( -d >= 0.0f )                       // len <= 0
        d = 1.0f;
    float inv = 1.0f / d;
    out[0] = in[0] * inv;
    out[1] = inv * in[1];
    out[2] = inv * in[2];
    return len;
}

// ─── 0x4a54d0 — project point `p` onto the line through `base` toward `dir`,
//     store the projected point in `out`.  (used by Curve_RemoveDegenerate.)
static void Curve_ProjectOntoLine( const float *p, float *out, const float *base, const float *dir )
{
    float e0 = p[0] - base[0];
    float e1 = p[1] - base[1];
    float e2 = p[2] - base[2];
    float lenSq = e2 * e2 + e0 * e0 + e1 * e1;
    if ( lenSq == 0.0f )
    {
        out[0] = base[0];
        out[1] = base[1];
        out[2] = base[2];
    }
    else
    {
        float d0 = dir[0] - base[0];
        float d1 = dir[1] - base[1];
        float d2 = dir[2] - base[2];
        float dot = d0 * e0 + d1 * e1 + d2 * e2;
        float t   = dot / lenSq;
        out[0] = e0 * t + base[0];
        out[1] = e1 * t + base[1];
        out[2] = t  * e2 + base[2];
    }
}

// ─── 0x431f40 — de Casteljau midpoint: out = (a + b) * 0.5 (7 floats + 4 colour
//     bytes averaged with >>1).
static void Curve_LerpHalf( curveVert_t *out, const curveVert_t *a, const curveVert_t *b )
{
    float       *o  = (float *)out;
    const float *fa = (const float *)a;
    const float *fb = (const float *)b;
    for ( int i = 0; i < 7; ++i )           // xyz(3) + st(2) + lightmap(2)
        o[i] = ( fb[i] + fa[i] ) * 0.5f;
    byte *oc = (byte *)&out->vert_color;
    const byte *ac = (const byte *)&a->vert_color;
    const byte *bc = (const byte *)&b->vert_color;
    oc[0] = (byte)( ( ac[0] + bc[0] ) >> 1 );
    oc[1] = (byte)( ( ac[1] + bc[1] ) >> 1 );
    oc[2] = (byte)( ( ac[2] + bc[2] ) >> 1 );
    oc[3] = (byte)( ( ac[3] + bc[3] ) >> 1 );
}

// ─── 0x432b20 — adaptive subdivision along COLUMNS (insert midpoint columns
//     until each curve segment's flatness is within tolerance).  Operates on the
//     scratch grid's row 0.  dims = {cols,rows}, indexMap may be null, tol = subdiv
//     tolerance, chordMax = the 999 chord limit.
static void Curve_SubdivideCols( int *dims, int *indexMap, int col0, float tol, float chordMax )
{
    while ( dims[0] + 2 < CURVE_GRID_DIM )
    {
        int allFlat = 0;
        if ( dims[1] > 0 )
        {
            // walk each row's three-point segment starting at (col0, row).
            curveVert_t *row = &s_curveScratch[0][col0];
            int r = 0;
            for ( ; r < dims[1]; ++r )
            {
                const float *p0 = row[r * CURVE_GRID_DIM + 0].xyz;
                const float *p1 = row[r * CURVE_GRID_DIM + 1].xyz;
                const float *p2 = row[r * CURVE_GRID_DIM + 2].xyz;
                float a0 = p1[0] - p0[0], a1f = p1[1] - p0[1], a2f = p1[2] - p0[2];
                if ( chordMax < (double)sqrt( a1f * a1f + a0 * a0 + a2f * a2f ) ) break;
                float b0 = p2[0] - p1[0], b1 = p2[1] - p1[1], b2 = p2[2] - p1[2];
                if ( chordMax < (double)sqrt( b1 * b1 + b0 * b0 + b2 * b2 ) ) break;
                float c0 = b0 - a0, c1 = b1 - a1f, c2 = b2 - a2f;
                float flat = (float)sqrt( c1 * c1 + c0 * c0 + c2 * c2 ) * 0.25f;
                if ( tol < (double)flat ) break;
            }
            allFlat = ( r == dims[1] );
        }
        if ( allFlat )
            break;

        dims[0] += 2;
        if ( indexMap )
        {
            for ( int j = dims[0] - 1; j > col0 + 3; --j )
                indexMap[j] = indexMap[j - 2];
            int v = indexMap[col0 + 1];
            indexMap[col0 + 3] = v;
            indexMap[col0 + 2] = v;
            indexMap[col0 + 1] = indexMap[col0];
        }

        if ( dims[1] > 0 )
        {
            for ( int row = 0; row < dims[1]; ++row )
            {
                curveVert_t *p = &s_curveScratch[row][col0 + 1];
                curveVert_t mid0, mid1, mid2;
                Curve_LerpHalf( &mid0, &p[0],  &p[-1] );
                Curve_LerpHalf( &mid1, &p[1],  &p[0]  );
                Curve_LerpHalf( &mid2, &mid1,  &mid0  );
                // shift the tail right by TWO to open the 2-column gap; then write
                // mid0/mid2/mid1 at p[0..2].
                for ( int k = dims[0] - 1; k > col0 + 3; --k )
                    s_curveScratch[row][k] = s_curveScratch[row][k - 2];
                p[0] = mid0;
                p[1] = mid2;
                p[2] = mid1;
            }
        }
    }
}

// ─── 0x432e20 — adaptive subdivision along ROWS (mirror of Curve_SubdivideCols
//     with the scratch grid's column/row strides swapped).
static void Curve_SubdivideRows( int *dims, int *indexMap, int row0, float tol, float chordMax )
{
    while ( dims[1] + 2 < CURVE_GRID_DIM )
    {
        int allFlat = 0;
        if ( dims[0] > 0 )
        {
            curveVert_t *col = &s_curveScratch[row0][0];
            int c = 0;
            for ( ; c < dims[0]; ++c )
            {
                const float *p0 = col[c + 0 * CURVE_GRID_DIM].xyz;
                const float *p1 = col[c + 1 * CURVE_GRID_DIM].xyz;
                const float *p2 = col[c + 2 * CURVE_GRID_DIM].xyz;
                float a0 = p1[0] - p0[0], a1f = p1[1] - p0[1], a2f = p1[2] - p0[2];
                if ( chordMax < (double)sqrt( a1f * a1f + a0 * a0 + a2f * a2f ) ) break;
                float b0 = p2[0] - p1[0], b1 = p2[1] - p1[1], b2 = p2[2] - p1[2];
                if ( chordMax < (double)sqrt( b1 * b1 + b0 * b0 + b2 * b2 ) ) break;
                float d0 = b0 - a0, d1 = b1 - a1f, d2 = b2 - a2f;
                float flat = (float)sqrt( d1 * d1 + d0 * d0 + d2 * d2 ) * 0.25f;
                if ( tol < (double)flat ) break;
            }
            allFlat = ( c == dims[0] );
        }
        if ( allFlat )
            break;

        dims[1] += 2;
        if ( indexMap )
        {
            for ( int j = dims[1] - 1; j > row0 + 3; --j )
                indexMap[j] = indexMap[j - 2];
            int v = indexMap[row0 + 1];
            indexMap[row0 + 3] = v;
            indexMap[row0 + 2] = v;
            indexMap[row0 + 1] = indexMap[row0];
        }

        if ( dims[0] > 0 )
        {
            for ( int col = 0; col < dims[0]; ++col )
            {
                curveVert_t *p = &s_curveScratch[row0 + 1][col];
                curveVert_t mid0, mid1, mid2;
                Curve_LerpHalf( &mid0, &p[0],                 &p[-CURVE_GRID_DIM] );
                Curve_LerpHalf( &mid1, &p[CURVE_GRID_DIM],    &p[0] );
                Curve_LerpHalf( &mid2, &mid1,                 &mid0 );
                // shift the tail right by TWO rows.
                for ( int k = dims[1] - 1; k > row0 + 3; --k )
                    s_curveScratch[k][col] = s_curveScratch[k - 2][col];
                p[0]                 = mid0;
                p[CURVE_GRID_DIM]    = mid2;
                p[2 * CURVE_GRID_DIM]= mid1;
            }
        }
    }
}

// ─── 0x433140 — subdivision SETUP: copy the control grid into the scratch grid,
//     seed the index maps, subdivide both axes, pack into a curvePatchDef_t.
static curvePatchDef_t *Curve_SubdivideSetup( int cols, int rows, const curveVert_t *src,
                                              float tol, float chordMax,
                                              int *colMap, int *rowMap )
{
    int dims[2] = { cols, rows };

    // copy src[col + row*cols] into scratch[row][col].
    for ( int col = 0; col < cols; ++col )
        for ( int row = 0; row < rows; ++row )
            s_curveScratch[row][col] = src[col + row * cols];

    if ( rowMap )
        for ( int i = 0; i < rows; ++i ) rowMap[i] = i;
    if ( colMap )
        for ( int j = 0; j < cols; ++j ) colMap[j] = j;

    // The do/while continue-test reads the LIVE (subdivision-grown) dims[0]/dims[1]
    // AFTER advancing the cursor by 2; the outer guard is the ORIGINAL count (cols/rows
    // > 2).  A `for (c+2 < dims[0])` is NOT equivalent: dims[0] grows during the body,
    // so the do/while runs one extra segment at the tail.
    if ( cols > 2 )
    {
        int c = 0;
        do { Curve_SubdivideCols( dims, colMap, c, tol, chordMax ); c += 2; }
        while ( c + 2 < dims[0] );
    }
    if ( rows > 2 )
    {
        int r = 0;
        do { Curve_SubdivideRows( dims, rowMap, r, tol, chordMax ); r += 2; }
        while ( r + 2 < dims[1] );
    }

    int w = dims[0], h = dims[1];
    size_t bytes = (size_t)44 * w * h;
    curvePatchDef_t *out = (curvePatchDef_t *)malloc( bytes + sizeof(curvePatchDef_t) );
    out->width  = w;
    out->height = h;
    out->verts  = (curveVert_t *)( out + 1 );
    for ( int row = 0; row < h; ++row )
        for ( int col = 0; col < w; ++col )
            out->verts[col + row * w] = s_curveScratch[row][col];
    return out;
}

// ─── 0x4329c0 — midpoint fixup: replace every odd interior vertex with the line
//     midpoint of its two even neighbours (smooths the inserted control points),
//     first along rows then along columns.
static void Curve_MidpointFixup( int cols, int rows, curveVert_t *verts )
{
    // along columns: for each col, fix odd rows from even neighbours.
    for ( int col = 0; col < cols; ++col )
    {
        if ( rows <= 1 ) continue;
        for ( int r = 1; r < rows; r += 2 )
        {
            curveVert_t *cur  = &verts[col + r * cols];
            curveVert_t *prev = &verts[col + (r - 1) * cols];
            curveVert_t *next = &verts[col + (r + 1) * cols];
            curveVert_t m0, m1;
            Curve_LerpHalf( &m1, next, cur );
            Curve_LerpHalf( &m0, prev, cur );
            Curve_LerpHalf( cur, &m0, &m1 );
        }
    }
    // along rows: for each row, fix odd cols from even neighbours.
    for ( int row = 0; row < rows; ++row )
    {
        if ( cols <= 1 ) continue;
        for ( int c = 1; c < cols; c += 2 )
        {
            curveVert_t *cur  = &verts[c + row * cols];
            curveVert_t *prev = &verts[c - 1 + row * cols];
            curveVert_t *next = &verts[c + 1 + row * cols];
            curveVert_t m0, m1;
            Curve_LerpHalf( &m1, next, cur );
            Curve_LerpHalf( &m0, prev, cur );
            Curve_LerpHalf( cur, &m0, &m1 );
        }
    }
}

// neighbour offset table (g_curveNeighborOffsets, 0x6DE534): 8-connected deltas in the
// binary's EXACT order.  entry[0] is applied to the column (ni=col+entry0) and entry[1]
// to the row (nj=row+entry1), so entry = (dcol, drow).
static const int s_curveNeighbors[8][2] =
{
    {  0,  1 }, {  1,  1 }, {  1,  0 }, {  1, -1 },
    {  0, -1 }, { -1, -1 }, { -1,  0 }, { -1,  1 },
};

// ─── 0x432190 — per-vertex NORMAL computation.  For each vertex, sum the
//     normalized cross products of adjacent edge pairs to its 8 neighbours
//     (with cyclic wrap when the patch is closed in that axis), then normalize.
static void Curve_ComputeNormals( curvePatchDef_t *curve )
{
    int width  = curve->width;
    int height = curve->height;

    // Detect cyclic closure:
    //   wrapWidth  — every row's first vs last COLUMN vertex coincide → the patch
    //                wraps in the WIDTH axis (cylinder seam); wraps `ni`.
    //   wrapHeight — every col's first vs last ROW vertex coincide → the patch
    //                wraps in the HEIGHT axis; wraps `nj`.
    int wrapWidth = 0;
    if ( height > 0 )
    {
        int r = 0;
        for ( ; r < height; ++r )
        {
            const float *a = curve->verts[r * width + 0].xyz;
            const float *b = curve->verts[r * width + (width - 1)].xyz;
            float d0 = a[0] - b[0], d1 = a[1] - b[1], d2 = a[2] - b[2];
            if ( (float)sqrt( d1 * d1 + d0 * d0 + d2 * d2 ) > 1.0f ) break;
        }
        wrapWidth = ( r == height );
    }
    int wrapHeight = 0;
    if ( width > 0 )
    {
        int c = 0;
        for ( ; c < width; ++c )
        {
            const float *a = curve->verts[0 * width + c].xyz;
            const float *b = curve->verts[(height - 1) * width + c].xyz;
            float d0 = a[0] - b[0], d1 = a[1] - b[1], d2 = a[2] - b[2];
            if ( (float)sqrt( d1 * d1 + d0 * d0 + d2 * d2 ) > 1.0f ) break;
        }
        wrapHeight = ( c == width );
    }

    for ( int col = 0; col < width; ++col )
    {
        for ( int row = 0; row < height; ++row )
        {
            const float *base = curve->verts[col + row * width].xyz;
            float dirs[8][3];
            int   have[8];

            for ( int n = 0; n < 8; ++n )
            {
                have[n] = 0;
                int di = s_curveNeighbors[n][0];
                int dj = s_curveNeighbors[n][1];
                // try up to 3 steps outward to find a non-degenerate neighbour edge.
                // The running step accumulators are UN-wrapped (col+step*dcol,
                // row+step*drow); the wrap is re-derived FRESH into temporaries each step
                // and the range check / vertex fetch use the wrapped temporaries — the
                // accumulator is never overwritten by the wrap (carrying the WRAPPED index
                // forward diverges on closed patches when a step re-crosses the seam
                // within 3 steps).
                int ci = col, rj = row;
                for ( int step = 1; step <= 3; ++step )
                {
                    ci += di;               // un-wrapped accumulator
                    rj += dj;
                    int ni = ci;            // wrap a FRESH copy each step
                    int nj = rj;
                    // cyclic wrap.
                    if ( wrapWidth )
                    {
                        if ( ni < 0 )            ni = width  + ni - 1;
                        else if ( ni >= width )  ni = ni - width + 1;
                    }
                    if ( wrapHeight )
                    {
                        if ( nj < 0 )            nj = height + nj - 1;
                        else if ( nj >= height ) nj = nj - height + 1;
                    }
                    if ( ni < 0 || ni >= width || nj < 0 || nj >= height )
                        break;
                    const float *nb = curve->verts[ni + nj * width].xyz;
                    float e[3] = { nb[0] - base[0], nb[1] - base[1], nb[2] - base[2] };
                    if ( Curve_NormalizeTo( dirs[n], e ) != 0.0f )
                    {
                        have[n] = 1;
                        break;
                    }
                }
            }

            // sum cross products of consecutive neighbour-direction pairs.
            float sum[3] = { 0.0f, 0.0f, 0.0f };
            for ( int n = 0; n < 8; ++n )
            {
                if ( !have[n] ) continue;
                int m = ( n + 1 ) & 7;
                if ( !have[m] ) continue;
                float cr[3], crn[3];
                // cr = dirs[m] × dirs[n] (cross arg order matters: it sets the normal's
                // SIGN).
                Vec3Cross( dirs[m], dirs[n], cr );          // (a, b, out=cr)
                if ( Curve_NormalizeTo( crn, cr ) != 0.0f )
                {
                    sum[0] += crn[0];
                    sum[1] += crn[1];
                    sum[2] += crn[2];
                }
            }
            Curve_NormalizeTo( curve->verts[col + row * width].normal, sum );
        }
    }
}

// ─── 0x4332d0 — remove DEGENERATE rows/cols (segments collapsed to < 0.1 units)
//     so the tessellated grid has no zero-area quads.  Works on the scratch grid.
static curvePatchDef_t *Curve_RemoveDegenerate( const curvePatchDef_t *in, int *colMap, int *rowMap )
{
    int cols = in->width;
    int rows = in->height;

    // load the incoming verts into the scratch grid (scratch[row][col]).
    for ( int col = 0; col < cols; ++col )
        for ( int row = 0; row < rows; ++row )
            s_curveScratch[row][col] = in->verts[col + row * cols];

    // collapse interior columns whose every-row chord is < 0.1.
    for ( int c = 1; c < cols - 1; )
    {
        float maxChord = 0.0f;
        for ( int row = 0; row < rows; ++row )
        {
            curveVert_t *cur  = &s_curveScratch[row][c];
            curveVert_t *prev = &s_curveScratch[row][c - 1];
            curveVert_t *next = &s_curveScratch[row][c + 1];
            float proj[3];
            // project CUR onto the prev→next line (the p and dir args are next/cur,
            // NOT cur/next).
            Curve_ProjectOntoLine( next->xyz, proj, prev->xyz, cur->xyz );
            float e0 = cur->xyz[0] - proj[0];
            float e1 = cur->xyz[1] - proj[1];
            float e2 = cur->xyz[2] - proj[2];
            float chord = (float)sqrt( e0 * e0 + e1 * e1 + e2 * e2 );
            if ( maxChord < chord ) maxChord = chord;
        }
        if ( maxChord < 0.1 )   // DOUBLE 0.1 (not float 0.1f); maxChord widens to double
        {
            // drop column c (shift cols c+1.. left by one).
            for ( int row = 0; row < rows; ++row )
                for ( int k = c; k < cols - 1; ++k )
                    s_curveScratch[row][k] = s_curveScratch[row][k + 1];
            if ( colMap )
                for ( int k = c; k < cols - 1; ++k )
                    colMap[k] = colMap[k + 1];
            --cols;
        }
        else
            ++c;
    }

    // collapse interior rows whose every-col chord is < 0.1.
    for ( int r = 1; r < rows - 1; )
    {
        float maxChord = 0.0f;
        for ( int col = 0; col < cols; ++col )
        {
            curveVert_t *cur  = &s_curveScratch[r][col];
            curveVert_t *prev = &s_curveScratch[r - 1][col];
            curveVert_t *next = &s_curveScratch[r + 1][col];
            float proj[3];
            // project CUR onto the prev→next line.
            Curve_ProjectOntoLine( next->xyz, proj, prev->xyz, cur->xyz );
            float e0 = cur->xyz[0] - proj[0];
            float e1 = cur->xyz[1] - proj[1];
            float e2 = cur->xyz[2] - proj[2];
            float chord = (float)sqrt( e0 * e0 + e1 * e1 + e2 * e2 );
            if ( maxChord < chord ) maxChord = chord;
        }
        if ( maxChord < 0.1 )   // DOUBLE 0.1 (not float 0.1f); maxChord widens to double
        {
            for ( int col = 0; col < cols; ++col )
                for ( int k = r; k < rows - 1; ++k )
                    s_curveScratch[k][col] = s_curveScratch[k + 1][col];
            if ( rowMap )
                for ( int k = r; k < rows - 1; ++k )
                    rowMap[k] = rowMap[k + 1];
            --rows;
        }
        else
            ++r;
    }

    size_t bytes = (size_t)44 * cols * rows;
    curvePatchDef_t *out = (curvePatchDef_t *)malloc( bytes + sizeof(curvePatchDef_t) );
    out->width  = cols;
    out->height = rows;
    out->verts  = (curveVert_t *)( out + 1 );
    for ( int row = 0; row < rows; ++row )
        for ( int col = 0; col < cols; ++col )
            out->verts[col + row * cols] = s_curveScratch[row][col];
    return out;
}

// ════════════════════════════════════════════════════════════════════════════
//  Patch_GenericMesh2  (0x438b60)  — build curveDef from the control grid.
//  `layer` = the texture layer whose S/T pair is copied into the render verts (the
//  caller passes g_qeglobals.current_edit_layer); colMapArg/rowMapArg = the index
//  maps (null on the render path).  Returns a freshly malloc'd
//  curvePatchDef_t (caller assigns it to patchMesh_t.curveDef and owns the free).
// ════════════════════════════════════════════════════════════════════════════
curvePatchDef_t *Patch_GenericMesh2( patchMesh_t *p, int layer, int *colMapArg, int *rowMapArg )
{
    int   width  = p->width;
    int   height = p->height;
    // Allocates unconditionally with NO null-return path — the copy loops below simply don't
    // execute when width/height<=0, and a valid (degenerate) curvePatchDef_t comes back
    // (unreachable for real patches, which have a minimum control grid).

    // flat control-vertex array cv[col + row*width].
    curveVert_t *cv = (curveVert_t *)operator new( (size_t)44 * height * width );

    for ( int row = 0; row < height; ++row )
    {
        for ( int col = 0; col < width; ++col )
        {
            const drawVert_t *src = &p->ctrl[col][row];
            curveVert_t      *dst = &cv[col + row * width];
            dst->xyz[0] = src->xyz[0];
            dst->xyz[1] = src->xyz[1];
            dst->xyz[2] = src->xyz[2];
            // layer-selected S/T pair (st[2*layer], st[2*layer+1]).
            const float *st = (const float *)&src->texCoord;
            dst->st[0]      = st[2 * layer];
            dst->st[1]      = st[2 * layer + 1];
            dst->lightmap[0]= 0.0f;
            dst->lightmap[1]= 0.0f;
            dst->normal[0]  = src->normal[0];
            dst->normal[1]  = src->normal[1];
            dst->normal[2]  = src->normal[2];
            dst->vert_color = src->vert_color;
        }
    }

    curvePatchDef_t *result;
    if ( ( p->type & PATCH_TERRAIN ) != 0 )
    {
        // terrain patches ARE their control grid (no subdivision).
        size_t bytes = (size_t)44 * width * height;
        result = (curvePatchDef_t *)malloc( bytes + sizeof(curvePatchDef_t) );
        result->width  = width;
        result->height = height;
        result->verts  = (curveVert_t *)( result + 1 );
        memcpy( result->verts, cv, bytes );
        Curve_ComputeNormals( result );
    }
    else
    {
        float subdiv = (float)p->subDivType;
        curvePatchDef_t *sub = Curve_SubdivideSetup( width, height, cv, subdiv, 999.0f,
                                                     (int *)(intptr_t)colMapArg,
                                                     (int *)(intptr_t)rowMapArg );
        Curve_MidpointFixup( sub->width, sub->height, sub->verts );
        Curve_ComputeNormals( sub );
        result = Curve_RemoveDegenerate( sub, (int *)(intptr_t)colMapArg,
                                         (int *)(intptr_t)rowMapArg );
        free( sub );
    }

    operator delete( cv );
    return result;
}

// ════════════════════════════════════════════════════════════════════════════
//  Patch_CalcVertColors  (0x4449e0)
//  Recomputes per-vertex colours from the texture's layered-material lightmap
//  mode. The layered material's unk_flags2 (filled by MaterialDef_06/15 off the
//  real registration) selects the mode: only (flags & 0x70) == 0x60
//  triggers a recompute. Every other case — no layered library entry
//  (lm == null → flags 0), or any non-lightmap mode — is a PASSTHROUGH: the
//  parsed vertex colours are kept verbatim (what the round-trip needs).
//
//  Recompute (only when the whole patch is still all-white = unauthored):
//  each control vertex's alpha becomes the luminance (R+G+B)/3 and its RGB is
//  forced to white — converting the parsed colour into a lightmap blend weight.
//  If ANY vertex already carries a non-white alpha the colours are treated as
//  authored and left untouched (early-out, also a passthrough).
//
//  Under the headless selftest the renderer is not up, so the material shim
//  keeps lm == null → flags 0 → passthrough → gate-neutral.  Bytes are summed
//  unsigned, signed /3, result fits a byte.
// ════════════════════════════════════════════════════════════════════════════
static int Patch_CalcVertColors( patchMesh_t *p )
{
    qtexture_s *lm = MaterialDef_GetLayeredMaterial( (MaterialDef *)&p->texture );
    int flags = lm ? lm->unk_flags2 : 0;

    if ( ( flags & 0x70 ) != 0x60 )             // not lightmap-alpha mode → keep
        return 0;

    // Pass 1 — if any vertex alpha is already != 0xFF, the colours were authored;
    // leave them.
    for ( int i = 0; i < p->width; i++ )
        for ( int j = 0; j < p->height; j++ )
            if ( (byte)p->ctrl[i][j].vert_color.a != 0xFF )
                return j;

    // Pass 2 — recompute: alpha = (R+G+B)/3, RGB = white.
    for ( int i = 0; i < p->width; i++ )
    {
        for ( int j = 0; j < p->height; j++ )
        {
            rgba_4byte *c = &p->ctrl[i][j].vert_color;
            int sum = (byte)c->r + (byte)c->g + (byte)c->b;
            c->a = (char)( sum / 3 );
            c->r = (char)0xFF;
            c->g = (char)0xFF;
            c->b = (char)0xFF;
        }
    }
    return p->height;
}

// ════════════════════════════════════════════════════════════════════════════
//  AddBrushForPatch  (0x438e40)
//  Creates the symbiont bounding brush for a patch, ties brush<->patch, and (only
//  when linking to world — NOT on the load path) links + builds it. The bbox brush
//  is never written to the .map (Brush_Write dispatches patches to Patch_Write and
//  returns), so its faces/bounds matter only for not crashing.
// ════════════════════════════════════════════════════════════════════════════
brush_t *AddBrushForPatch( patchMesh_t *p, entity_s *world_ent )
{
    float mins[3], maxs[3];
    Patch_CalcBounds( mins, maxs, p, true );

    MaterialDef md;
    memset( &md, 0, sizeof( md ) );
    md.lyrMtl = p->texture.lyrMtl;
    md.radMtl = p->texture.radMtl;

    qtexture_s *lm = MaterialDef_GetLayeredMaterial( &md );
    int w = lm ? lm->width  : 512;
    int h = lm ? lm->height : 512;

    texdef_sub_t *texSlot = &md.mat_texDef + LayerMat::GetCurrentLayer( &md );
    texSlot->size[0] = (double)w * 0.25;
    texSlot->size[1] = 0.25 * (double)h;

    brush_t *b = Brush_Alloc( &md, nullptr );
    Brush_Create( mins, maxs, b, nullptr );

    for ( unsigned i = 0; i < (unsigned)b->faceCount; ++i )
    {
        b->faces[i].contents  = p->contents;
        b->faces[i].toolflags = p->flags;
    }

    b->patch    = p;
    ++p->version;
    p->pSymbiot = (entity_brush_s *)b;

    if ( world_ent )
    {
        // NOT reached on the load path (Patch_ParseMesh passes world_ent = null;
        // ParseEntity links the returned brush itself).
        Entity_LinkBrush( b, world_ent );
        Brush_BuildWindings( b, 1 );
        if ( g_qeglobals.d_select_mode == sel_vertex ||
             g_qeglobals.d_select_mode == sel_edge )
            SetupVertexSelection();
        MarkMapModified();
        ++b->version;
    }
    return b;
}

// ════════════════════════════════════════════════════════════════════════════
//  Patch_ParseVert  (0x4446f0)  — parse ONE control vertex.
//  Grammar:  v <x> <y> <z>  [c <b> <g> <r> <a>]  t <s0> <t0> <s1> <t1>  [f <edge>]
//  Texcoords are stored ÷1024 (the file holds the ×1024 integer form).
// ════════════════════════════════════════════════════════════════════════════
static char Patch_ParseVert( const char **text, drawVert_t *v )
{
    iassert( v );

    // "v"
    if ( strcmp( Com_Parse( text )->token, "v" ) )
        return 0;

    // xyz (same line)
    v->xyz[0] = (float)atof( Com_ParseOnLine( text )->token );
    v->xyz[1] = (float)atof( Com_ParseOnLine( text )->token );
    v->xyz[2] = (float)atof( Com_ParseOnLine( text )->token );

    const char *tok = Com_Parse( text )->token;
    if ( !strcmp( tok, "c" ) )
    {
        // wire order is b g r a
        v->vert_color.b = (char)atol( Com_ParseOnLine( text )->token );
        v->vert_color.g = (char)atol( Com_ParseOnLine( text )->token );
        v->vert_color.r = (char)atol( Com_ParseOnLine( text )->token );
        v->vert_color.a = (char)atol( Com_ParseOnLine( text )->token );
        tok = Com_Parse( text )->token;
    }
    else
    {
        *(unsigned int *)&v->vert_color = 0xFFFFFFFF;   // white
    }

    if ( strcmp( tok, "t" ) )
        return 0;

    // texCoord is a flat float[6]: st[0..1]=tex, st[2..3]=lightmap, st[4..5]=unused.
    float *st = (float *)&v->texCoord;
    for ( int i = 0; i < 3; ++i )
    {
        if ( i == 2 )
        {
            st[4] = 0.0f;
            st[5] = 0.0f;
        }
        else
        {
            float s = (float)( (float)atof( Com_ParseOnLine( text )->token ) * 0.0009765625 );
            float t = (float)( (float)atof( Com_ParseOnLine( text )->token ) * 0.0009765625 );
            iassert( isfinite( s ) );
            iassert( isfinite( t ) );
            st[2 * i]     = s;
            st[2 * i + 1] = t;
        }
    }

    // optional "f <turned_edge>"
    if ( !strcmp( Com_Parse( text )->token, "f" ) )
    {
        v->turned_edge = (int)atol( Com_ParseOnLine( text )->token );
        return 1;
    }
    v->turned_edge = 0;
    Com_UngetToken();
    return 1;
}

// ════════════════════════════════════════════════════════════════════════════
//  Patch_ParseMesh  (0x444ac0)  — parse a `mesh`/`curve` block.
//  Called by Brush_Parse for the patch branch. `isMesh` = the block matched the
//  terrain variant ("mesh"/"patchTerrainDef3") → patchMesh_t gets PATCH_TERRAIN.
//  Returns the symbiont brush (brush->patch set), or null on parse error.
//  Only the iwmap-4 (version >= 4) grammar is ported — every stock test map is
//  version 4. The legacy (< 4) path needs the subdivision/version-0 material
//  remap helpers (Patch_ResampleCurveST / Patch_TerrainTexProject / Mtl_NormalizeName), all out of round-trip scope.
// ════════════════════════════════════════════════════════════════════════════
brush_t *Patch_ParseMesh( const char **text, int version, int isMesh )
{
    // mesh-block open "{"
    if ( Com_Parse( text )->token[0] != '{' )
        return nullptr;

    // KIWI: checked before MakeNewPatch - the patch leaked on this path
    if ( version < 4 )
    {
        Error( "pmesh: legacy patch version < 4 not supported (round-trip subset); "
               "needs Patch_ResampleCurveST / Patch_TerrainTexProject / Mtl_NormalizeName" );
        return nullptr;
    }

    patchMesh_t *p = MakeNewPatch();

    char layerBuf[1024];
    Map_ParseEntityLayerKey( text, "000_Global", layerBuf, "layer" );
    // optional `contents <names>;` / `toolFlags <names>;` - KIWI: any number, any order
    // (newer CoD Radiants write one line per flag; engine_stubs.cpp Map_ParseFlagLines)
    {
        extern void Map_ParseFlagLines( const char **text, int *contents, int *toolFlags );
        int contents = 0, toolFlags = 0;
        Map_ParseFlagLines( text, &contents, &toolFlags );
        p->contents = contents;
        p->flags    = toolFlags;
    }

    // material names. spaceDelimited=1 so '/' in a material path isn't a delimiter;
    // it covers BOTH the texture and lightmap reads, then reset to 0 for smoothing.
    ParseThreadInfo *pi = Com_GetParseThreadInfo();
    pi->parseInfo[pi->parseInfoNum].spaceDelimited = 1;
    SetMaterial( Com_Parse( text )->token, &p->texture );
    SetMaterial( Com_Parse( text )->token, &p->lightmap );
    pi->parseInfo[pi->parseInfoNum].spaceDelimited = 0;

    char smoothBuf[1024];
    Map_ParseEntityLayerKey( text, "smoothing_smooth", smoothBuf, "smoothing" );
    SetMaterial( smoothBuf, &p->smoothing );

    // KIWI texture layers: "kiwilayer <slot> <material>" lines, any count, any order.
    memset( p->kiwiLayer, 0, sizeof( p->kiwiLayer ) );
    for ( ;; )
    {
        parseInfo_t *tok = Com_Parse( text );
        if ( strcmp( tok->token, "kiwilayer" ) )
        {
            Com_UngetToken();
            break;
        }
        const int slot = (int)atol( Com_ParseOnLine( text )->token );
        pi->parseInfo[pi->parseInfoNum].spaceDelimited = 1;
        parseInfo_t *nameTok = Com_ParseOnLine( text );
        pi->parseInfo[pi->parseInfoNum].spaceDelimited = 0;
        if ( slot >= 0 && slot < 4 )
        {
            strncpy( p->kiwiLayer[slot], nameTok->token, 63 );
            p->kiwiLayer[slot][63] = 0;
        }
    }

    // params:  <width> <height> <size> <subDivType>
    p->width  = Com_ParseInt( text );
    p->height = (int)atol( Com_ParseOnLine( text )->token );
    // KIWI: the binary (0x444AC0) reads the grid straight into ctrl[16][16] with no clamp,
    // so a hand-edited or foreign .map with "24 24" corrupts the heap on load.  Reject it
    // as a parse error instead (the writer never emits anything outside 2..16).
    if ( p->width < 2 || p->width > 16 || p->height < 2 || p->height > 16 )
        Com_ScriptErrorDrop( "patch size %i x %i is outside the 2..16 control-point range\n",
                             p->width, p->height );

    if ( isMesh )
        p->type = PATCH_TERRAIN;

    float sizeVal = (float)atof( Com_ParseOnLine( text )->token );
    *(float *)&p->size_of_struct_0x504C = sizeVal;
    p->bDirty = ( sizeVal != 0.0f );

    int sdt = (int)atol( Com_ParseOnLine( text )->token );
    p->subDivType = ( sdt < 1 ) ? 1 : sdt;

    // control grid:  for each column "(" <height vertices> ")"
    for ( int col = 0; col < p->width; ++col )
    {
        Com_MatchToken( text, "(", 0 );
        drawVert_t *colBase = p->ctrl[col];
        for ( int row = 0; row < p->height; ++row )
            Patch_ParseVert( text, &colBase[row] );

        parseInfo_t *t = Com_Parse( text );
        if ( strcmp( t->token, ")" ) )
            Com_ScriptErrorDrop( "MatchToken: got '%s', expected '%s'\n", t->token, ")" );
    }

    // mesh-block close "}"
    parseInfo_t *closeTok = Com_Parse( text );
    if ( strcmp( closeTok->token, "}" ) )
        Com_ScriptErrorDrop( "MatchToken: got '%s', expected '%s'\n", closeTok->token, "}" );

    if ( version <= 4 )
        Patch_CalcVertColors( p );

    if ( p->curveDef )
        free( p->curveDef );
    p->curveDef = Patch_GenericMesh2( p, g_qeglobals.current_edit_layer, 0, 0 );

    brush_t *b = AddBrushForPatch( p, nullptr );
    if ( b )
    {
        if ( b->parent_layer_string )
            free( b->parent_layer_string );
        size_t n  = strlen( layerBuf );
        char  *lc = (char *)operator new( n + 1 );
        memcpy( lc, layerBuf, n + 1 );
        b->parent_layer_string = lc;
    }
    return b;
}

// ════════════════════════════════════════════════════════════════════════════
//  Patch write path.  Brush_Write dispatches `brush->patch` to Patch_Write.
//  The writer handle is a triple-pointer to a (ctx, fmt, ...) printf-like fn; the
//  ctx is the handle pointer cast to int (same convention as brush.cpp Brush_Write).
// ════════════════════════════════════════════════════════════════════════════
typedef int  WriteFunc_t( void *ctx, const char *fmt, ... );
typedef WriteFunc_t **WriteWriter_t;
#define WRITE( w, ... ) ( ( **(w) )( (w), __VA_ARGS__ ) )

// VC7.1-compatible "%.8g" (round-half-away-from-zero + 3-digit exponent). Defined
// once in brush.cpp (the brush planept/texdef writer); shared here so patch verts
// format identically to brush planepts.
extern const char *Fmt8g( char *buf, double v );

// ─── Patch_WriteVert  (0x4456f0) — write ONE control vertex.
static void Patch_WriteVert( drawVert_t *v, WriteWriter_t writer )
{
    iassert( v );
    iassert( isfinite( v->xyz[0] ) && isfinite( v->xyz[1] ) && isfinite( v->xyz[2] ) );

    char bx[64], by[64], bz[64];
    WRITE( writer, "\tv %s %s %s",
           Fmt8g( bx, v->xyz[0] ), Fmt8g( by, v->xyz[1] ), Fmt8g( bz, v->xyz[2] ) );

    byte r = (byte)v->vert_color.r;
    byte g = (byte)v->vert_color.g;
    byte b = (byte)v->vert_color.b;
    byte a = (byte)v->vert_color.a;
    if ( r != 0xFF || g != 0xFF || b != 0xFF || a != 0xFF )
        WRITE( writer, " c %i %i %i %i", b, g, r, a );   // wire order: b g r a

    WRITE( writer, " t" );

    const float *st = (const float *)&v->texCoord;   // flat float[6]
    for ( int i = 0; i < 3; ++i )
    {
        if ( i != 2 )
        {
            float s = (float)( st[2 * i]     * 1024.0 );
            float t = (float)( st[2 * i + 1] * 1024.0 );
            iassert( isfinite( s ) );
            iassert( isfinite( t ) );
            char bs[64], bt[64];
            WRITE( writer, " %s %s", Fmt8g( bs, s ), Fmt8g( bt, t ) );
        }
    }

    if ( v->turned_edge )
        WRITE( writer, " f %i", v->turned_edge );

    WRITE( writer, "\n" );
}

// ─── Patch_Write  (0x4458f0) — write a patch as a `mesh`/`curve` block.  Called by
//     Brush_Write.  Layer comes from the symbiont brush's parent_layer_string.
int Patch_Write( WriteWriter_t writer, patchMesh_t *p )
{
    if ( ( p->type & PATCH_TERRAIN ) != 0 )
        WRITE( writer, " {\n  mesh\n  {\n" );
    else
        WRITE( writer, " {\n  curve\n  {\n" );

    const char *layerName = ( (brush_t *)p->pSymbiot )->parent_layer_string;
    if ( strcmp( layerName, "000_Global" ) )
    {
        MapLoad_ParseBrush_Layer( writer, layerName );
    }

    MapLoad_ParseBrush_Content( "contents",  writer,
                                p->contents, contents_table );
    MapLoad_ParseBrush_Content( "toolFlags", writer,
                                p->flags,    toolflags_table );

    // texture / lightmap / smoothing NAMES. Must go through Materialdef_GetName, NOT a raw
    // (char*)lyrMtl cast: when the renderer is up, SetMaterial resolves a real material and
    // stores it in lyrMtl — but pseudo-materials ("$default", "lightmap_gray",
    // "smoothing_smooth") aren't layered assets, so lyrMtl comes back NULL and the name lives
    // in radMtl->name. A direct cast reads NULL and strcmp() crashes when copying a patch.
    const char *texName = (const char *)Materialdef_GetName( (MaterialDef *)&p->texture );
    WRITE( writer, "   %s\n", texName );
    const char *lmName = (const char *)Materialdef_GetName( (MaterialDef *)&p->lightmap );
    WRITE( writer, "   %s\n", lmName );
    const char *smName = (const char *)Materialdef_GetName( (MaterialDef *)&p->smoothing );
    if ( strcmp( smName, "smoothing_smooth" ) )
        WRITE( writer, "   smoothing %s\n", smName );
    // KIWI texture layers (patchMesh_t.kiwiLayer): one optional line per used slot.
    for ( int slot = 0; slot < 4; ++slot )
        if ( p->kiwiLayer[slot][0] )
            WRITE( writer, "   kiwilayer %i %s\n", slot, p->kiwiLayer[slot] );

    // The binary used plain "%g" for the LOD size (6 sig figs).  KIWI: Fmt8g - a scaled patch's
    // 21.333334 saved as 21.3333; stock integer sizes (0 in blackout) print as before.
    float sizeVal = p->bDirty ? *(float *)&p->size_of_struct_0x504C : 0.0f;
    char sizeBuf[40];
    WRITE( writer, "   %i %i %s %i\n", p->width, p->height, Fmt8g( sizeBuf, (double)sizeVal ), p->subDivType );

    for ( int col = 0; col < p->width; ++col )
    {
        WRITE( writer, "   (\n" );
        drawVert_t *colBase = p->ctrl[col];
        for ( int row = 0; row < p->height; ++row )
            Patch_WriteVert( &colBase[row], writer );
        WRITE( writer, "   )\n" );
    }

    return WRITE( writer, "  }\n }\n" );
}

// ════════════════════════════════════════════════════════════════════════════
//  PATCH CREATE + GRID MANIPULATION (the control-grid DATA layer)
//    * create a patch from a brush (Patch_BrushToMesh / Patch_GenericMesh) - fill the
//      control grid + lay LINEAR per-control texCoords via Patch_Naturalize2, then
//      REFINE those texCoords with Patch_ResampleCurveST / Patch_TerrainTexProject (they
//      tessellate the patch and re-sample S/T off the render mesh).
//    * grow/shrink the grid (Patch_InsertColumn/Row, Patch_RemoveColumn/Row) and rebuild
//      via Patch_Rebuild (texCoord refine + bbox + curveDef).
// ════════════════════════════════════════════════════════════════════════════

static const double kOneThird = 0.3333333432674408;   // 1/3 as a float

// ── Patch_WidthDistanceTo (0x438FA0) / Patch_HeightDistanceTo (0x439040) ──────
//    Cumulative 3D distance walking the first row's columns 0..n-1 (width) or the
//    first column's rows 0..n-1 (height). Used by Patch_Naturalize2 for linear S/T.
// Accumulates in a FLOAT: each segment's `sqrt` result is rounded to a float and the
// running total is a float too.  A double accumulator would carry more precision than
// the binary and shift the laid S/T coords — keep float.
static double Patch_WidthDistanceTo( int n, patchMesh_t *p )
{
    float sum = 0.0f;
    for ( int i = 0; i < n; ++i )
    {
        float dx = p->ctrl[i + 1][0].xyz[0] - p->ctrl[i][0].xyz[0];
        float dy = p->ctrl[i + 1][0].xyz[1] - p->ctrl[i][0].xyz[1];
        float dz = p->ctrl[i + 1][0].xyz[2] - p->ctrl[i][0].xyz[2];
        float seg = (float)sqrt( (double)( dx * dx + dy * dy + dz * dz ) );
        sum = seg + sum;
    }
    return sum;
}

static double Patch_HeightDistanceTo( int n, patchMesh_t *p )
{
    float sum = 0.0f;
    for ( int j = 0; j < n; ++j )
    {
        float dx = p->ctrl[0][j + 1].xyz[0] - p->ctrl[0][j].xyz[0];
        float dy = p->ctrl[0][j + 1].xyz[1] - p->ctrl[0][j].xyz[1];
        float dz = p->ctrl[0][j + 1].xyz[2] - p->ctrl[0][j].xyz[2];
        float seg = (float)sqrt( (double)( dx * dx + dy * dy + dz * dz ) );
        sum = seg + sum;
    }
    return sum;
}

// ── Patch_Naturalize2 (0x439840) — lay LINEAR per-control-point S/T on `layer`.
//    S advances with cumulative width-distance / (texWidth * scaleS); T with negated
//    cumulative height-distance / (texHeight * scaleT). Self-contained (no curveDef).
//    For the degenerate headless material, GetLayeredMaterial→null → 512×512 default.
static void Patch_Naturalize2( patchMesh_t *p, int layer, float scaleS, float scaleT )
{
    qtexture_s *lm = MaterialDef_GetLayeredMaterial( (MaterialDef *)( &p->texture + layer ) );
    int w = lm ? lm->width  : 512;
    int h = lm ? lm->height : 512;

    // Both reciprocals AND the distance accumulators are 32-bit FLOATs, and the S-product
    // widthDist*sScale is rounded to a float intermediate hoisted out of the inner loop
    // before storing.  Double here carries up to 1 ULP of extra precision into the
    // serialized control-point texCoords on the Naturalize/Redisperse paths.
    float sScale = (float)(  1.0 / ( (double)w * scaleS ) );
    float tScale = (float)( -1.0 / ( (double)h * scaleT ) );

    float widthDist = 0.0f;
    for ( int i = 0; i < p->width; ++i )
    {
        float heightDist = 0.0f;
        float sCoord = widthDist * sScale;   // float intermediate, once per row
        for ( int j = 0; j < p->height; ++j )
        {
            float *st = &p->ctrl[i][j].texCoord.st[2 * layer];
            st[0] = sCoord;
            st[1] = heightDist * tScale;
            heightDist = (float)Patch_HeightDistanceTo( j + 1, p );
        }
        widthDist = (float)Patch_WidthDistanceTo( i + 1, p );
    }
    if ( layer == 1 )
        p->bDirty = 0;
}

// ── InterpolateInteriorPoints (0x437BB0) — fill the interior EVEN control points of
//    a freshly-created cylinder/square grid: each even column w's points become the
//    average of the two flanking odd columns (wPrev = w-1 / wrap; wNext = w+1 / wrap).
//    Skips a point whose drawVert flag byte (turned_edge low byte) has bit 1 set —
//    always 0 on a freshly-made patch (the only caller), so the average always runs.
static void InterpolateInteriorPoints( patchMesh_t *p )
{
    for ( int w = 0; w < p->width; w += 2 )
    {
        int wNext = ( w == p->width - 1 ) ? 1 : ( ( w + 1 ) % p->width );
        int wPrev = ( w == 0 ) ? ( p->width - 2 ) : ( w - 1 );
        for ( int h = 0; h < p->height; ++h )
        {
            if ( ( (byte)p->ctrl[w][h].turned_edge & 2 ) == 0 )
            {
                for ( int k = 0; k < 3; ++k )
                    p->ctrl[w][h].xyz[k] =
                        ( p->ctrl[wNext][h].xyz[k] + p->ctrl[wPrev][h].xyz[k] ) * 0.5f;
            }
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  Patch_BrushToMesh  (0x43ACC0)  — create a curve patch (cylinder / bevel / endcap /
//  square / cone) from the single selected brush, then replace the brush with it.
// ════════════════════════════════════════════════════════════════════════════
void Patch_BrushToMesh( char bCone, byte bBevel, byte bEndcap, char bSquare )
{
    if ( !QE_SingleBrush() )
        return;

    brush_t  *def   = selected_brushes.next->def;
    entity_s *owner = selected_brushes.next->owner;
    const float *mins = def->mins;
    const float *maxs = def->maxs;

    patchMesh_t *p = MakeNewPatch();
    p->height = 3;
    p->type   = PATCH_CYLINDER;

    if ( ( bSquare == 0 ) & bBevel )
    {
        p->type  = PATCH_BEVEL;
        p->width = 3;
        int step  = (int)( ( maxs[2] - mins[2] ) * 0.5 );
        int z     = (int)mins[2];
        for ( int i = 0; i < p->height; ++i )
        {
            p->ctrl[0][i].xyz[0] = mins[0]; p->ctrl[0][i].xyz[1] = mins[1]; p->ctrl[0][i].xyz[2] = (float)z;
            p->ctrl[1][i].xyz[0] = maxs[0]; p->ctrl[1][i].xyz[1] = mins[1]; p->ctrl[1][i].xyz[2] = (float)z;
            p->ctrl[2][i].xyz[0] = maxs[0]; p->ctrl[2][i].xyz[1] = maxs[1]; p->ctrl[2][i].xyz[2] = (float)z;
            z += step;
        }
    }
    else if ( ( bSquare == 0 ) & bEndcap )
    {
        p->type  = PATCH_ENDCAP;
        p->width = 5;
        int step  = (int)( ( maxs[2] - mins[2] ) * 0.5 );
        int z     = (int)mins[2];
        for ( int i = 0; i < p->height; ++i )
        {
            p->ctrl[0][i].xyz[0] = mins[0]; p->ctrl[0][i].xyz[1] = mins[1]; p->ctrl[0][i].xyz[2] = (float)z;
            p->ctrl[1][i].xyz[0] = mins[0]; p->ctrl[1][i].xyz[1] = maxs[1]; p->ctrl[1][i].xyz[2] = (float)z;
            p->ctrl[2][i].xyz[0] = (maxs[0] - mins[0]) * 0.5f + mins[0];
            p->ctrl[2][i].xyz[1] = maxs[1]; p->ctrl[2][i].xyz[2] = (float)z;
            p->ctrl[3][i].xyz[0] = maxs[0]; p->ctrl[3][i].xyz[1] = maxs[1]; p->ctrl[3][i].xyz[2] = (float)z;
            p->ctrl[4][i].xyz[0] = maxs[0]; p->ctrl[4][i].xyz[1] = mins[1]; p->ctrl[4][i].xyz[2] = (float)z;
            z += step;
        }
    }
    else
    {
        p->width = 9;
        p->ctrl[1][0].xyz[0] = mins[0]; p->ctrl[1][0].xyz[1] = mins[1];
        p->ctrl[3][0].xyz[0] = maxs[0]; p->ctrl[3][0].xyz[1] = mins[1];
        p->ctrl[5][0].xyz[0] = maxs[0]; p->ctrl[5][0].xyz[1] = maxs[1];
        p->ctrl[7][0].xyz[0] = mins[0]; p->ctrl[7][0].xyz[1] = maxs[1];
        for ( int i = 1; i < p->width - 1; i += 2 )
        {
            p->ctrl[i][0].xyz[2] = mins[2];
            p->ctrl[i][2].xyz[0] = p->ctrl[i][0].xyz[0];
            p->ctrl[i][2].xyz[1] = p->ctrl[i][0].xyz[1];
            p->ctrl[i][2].xyz[2] = maxs[2];
            for ( int k = 0; k < 3; ++k )
                p->ctrl[i][1].xyz[k] = ( p->ctrl[i][0].xyz[k] + p->ctrl[i][2].xyz[k] ) * 0.5f;
        }
        InterpolateInteriorPoints( p );

        if ( bSquare )
        {
            if ( bBevel )
            {
                for ( int i = 0; i < p->height; ++i )
                {
                    p->ctrl[2][i].xyz[0] = p->ctrl[1][i].xyz[0];
                    p->ctrl[2][i].xyz[1] = p->ctrl[1][i].xyz[1];
                    p->ctrl[2][i].xyz[2] = p->ctrl[1][i].xyz[2];
                    p->ctrl[6][i].xyz[0] = p->ctrl[7][i].xyz[0];
                    p->ctrl[6][i].xyz[1] = p->ctrl[7][i].xyz[1];
                    p->ctrl[6][i].xyz[2] = p->ctrl[7][i].xyz[2];
                }
            }
            else if ( bEndcap )
            {
                for ( int i = 0; i < p->height; ++i )
                {
                    p->ctrl[4][i].xyz[0] = p->ctrl[5][i].xyz[0];
                    p->ctrl[4][i].xyz[1] = p->ctrl[5][i].xyz[1];
                    p->ctrl[4][i].xyz[2] = p->ctrl[5][i].xyz[2];
                    p->ctrl[2][i].xyz[0] = p->ctrl[1][i].xyz[0];
                    p->ctrl[2][i].xyz[1] = p->ctrl[1][i].xyz[1];
                    p->ctrl[2][i].xyz[2] = p->ctrl[1][i].xyz[2];
                    p->ctrl[6][i].xyz[0] = p->ctrl[7][i].xyz[0];
                    p->ctrl[6][i].xyz[1] = p->ctrl[7][i].xyz[1];
                    p->ctrl[6][i].xyz[2] = p->ctrl[7][i].xyz[2];
                    p->ctrl[7][i].xyz[0] = p->ctrl[8][i].xyz[0];
                    p->ctrl[7][i].xyz[1] = p->ctrl[8][i].xyz[1];
                    p->ctrl[7][i].xyz[2] = p->ctrl[8][i].xyz[2];
                }
            }
            else
            {
                for ( int i = 0; i < p->width - 1; ++i )
                    for ( int j = 0; j < p->height; ++j )
                    {
                        p->ctrl[i][j].xyz[0] = p->ctrl[i + 1][j].xyz[0];
                        p->ctrl[i][j].xyz[1] = p->ctrl[i + 1][j].xyz[1];
                        p->ctrl[i][j].xyz[2] = p->ctrl[i + 1][j].xyz[2];
                    }
                for ( int j = 0; j < p->height; ++j )
                {
                    p->ctrl[8][j].xyz[0] = p->ctrl[0][j].xyz[0];
                    p->ctrl[8][j].xyz[1] = p->ctrl[0][j].xyz[1];
                    p->ctrl[8][j].xyz[2] = p->ctrl[0][j].xyz[2];
                }
            }
        }
    }

    // inherit the source brush's face material (texture + lightmap layers).
    p->texture  = *(patchMesh_material *)&def->faces->mtldef[0].lyrMtl;
    p->lightmap = *(patchMesh_material *)&def->faces->mtldef[1].lyrMtl;

    // Order: Naturalize2 → the two texCoord refinement passes → curveDef → cone reshape →
    // link.  Cone reshape AFTER Naturalize2 (the cone collapses each column's apex point
    // to the bbox centre).
    // BOTH scale args are random_texture_stuff[0].sampleSize (the layer-0 default tex
    // scale) — NOT a hardcoded 0.25.  The GUI seeds sampleSize on init; headless the test
    // harness only memcpy's the MaterialDef into layer 0 and leaves sampleSize zero, which
    // would make sScale=1/(w*0)=inf → NaN S/T → a Patch_Write IS_NAN assert.  So the
    // editor's 0.25 default is used only when it is unseeded (== 0).
    float natScale = g_qeglobals.random_texture_stuff[0].sampleSize;
    if ( natScale == 0.0f )
        natScale = 0.25f;
    Patch_Naturalize2( p, 0, natScale, natScale );
    // texCoord refinement passes — identical shape to the sibling Patch_GenericMesh:
    // layer 1 at 16.0, then size = 16.0 / bDirty = 1, then layer 2 at 0.25.  Without them a
    // newly-created cylinder/bevel/endcap/cone has only the linear Naturalize2 S/T.
    // bDirty also gates the `size` field in Patch_Write (a created patch would otherwise
    // save `size 0` instead of `size 16`).
    const bool terrain = ( p->type & PATCH_TERRAIN ) != 0;
    if ( terrain ) Patch_TerrainTexProject( p, 1, 16.0f );
    else           Patch_ResampleCurveST( p, 1, 16.0f );
    *(float *)&p->size_of_struct_0x504C = 16.0f;
    p->bDirty = 1;
    if ( terrain ) Patch_TerrainTexProject( p, 2, 0.25f );
    else           Patch_ResampleCurveST( p, 2, 0.25f );
    if ( p->curveDef )
        free( p->curveDef );
    // builds the tessellated curveDef render mesh (Bezier subdivision for the
    // cylinder/bevel/endcap/cone control grid) so the new patch renders as wireframe.
    p->curveDef = Patch_GenericMesh2( p, g_qeglobals.current_edit_layer, 0, 0 );
    ++p->version;

    if ( bCone )
    {
        p->type = PATCH_CONE;
        float xc = ( maxs[0] + mins[0] ) * 0.5f;
        float yc = ( maxs[1] + mins[1] ) * 0.5f;
        for ( int i = 0; i < p->width; ++i )
        {
            p->ctrl[i][2].xyz[0] = xc;
            p->ctrl[i][2].xyz[1] = yc;
        }
    }

    brush_t   *pdef = AddBrushForPatch( p, (entity_s *)owner->def );
    selbrush_t *inst = Brush_AddToList( pdef, owner );
    if ( inst->next || inst->prev )
        Error( "Brush_AddToList: already linked" );
    if ( &active_brushes == &selected_brushes )
        Brush_AddToList2( inst );
    else
    {
        inst->next = active_brushes.next;
        active_brushes.next->prev = inst;
        active_brushes.next = inst;
        inst->prev = &active_brushes;
    }

    Select_Delete();
    Select_Brush( inst, 1, 1, 0 );
}

// ════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND Q) — Patch_KiwiFinishNew
//
//  The TEXTURE + TESSELLATION TAIL every patch primitive in this file runs after
//  it has filled ctrl[][], factored out so a KIWI command can create a patch
//  through the SAME sequence instead of carrying a second copy of it.
//
//  It is the block at 0x43b18c-0x43b204 (Patch_BrushToMesh above, lines
//  "float natScale …" through "++p->version") lifted VERBATIM — same order, same
//  constants, same bDirty handling.  The sibling creators Patch_GenericMesh
//  (:1553-1570) and Create_Terrain (:1655-1672) run the identical block, so this
//  is the shape all three agree on and not one of them generalised.
//
//  It exists because the two workers it needs — Patch_Naturalize2 (0x439840) and
//  Patch_ResampleCurveST (0x439580) — are file-static and must stay that way: they are ported
//  internals with IDA-pinned float behaviour, and widening them into the public
//  header to serve one caller would be worse than one forwarder.
//
//  NOT included, deliberately: the ctrl[][] fill (the caller's whole job), the
//  material inheritance (the caller knows which face it is copying), and the
//  AddBrushForPatch / Brush_AddToList / Brush_AddToList2 landing (the caller owns
//  the undo bracket those must sit inside).
//
//  NO PORTED CALLER IS CHANGED.  Patch_BrushToMesh et al keep their inlined copy,
//  so the ported creation paths are byte-for-byte what they were.
// ════════════════════════════════════════════════════════════════════════════
// ════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND AJ, ITEM 2) — Patch_KiwiFinishNewLike
//
//  USER DIRECTIVE, verbatim: "Also the Texture should be fixed so it looks
//  natural with the parent solid edge area."
//
//  WHAT WAS WRONG, and it is a SCALE, not a projection.  Patch_Naturalize2 lays S
//  from the cumulative WIDTH DISTANCE in world units (Patch_WidthDistanceTo,
//  :1183) divided by `texWidth * scaleS`, so `texWidth * scaleS` IS the number of world
//  units one full texture repeat covers.  A brush FACE expresses the same quantity
//  directly as its texdef `size[0]` (brush.cpp:3327 / pmesh.cpp:855 both seed it
//  as `texWidth * 0.25`, i.e. exactly this product at the default sample size).
//  Patch_KiwiFinishNew hardcoded `random_texture_stuff[0].sampleSize` instead, so a
//  patch created against a wall whose texdef had been scaled — which is every wall
//  a mapper has actually worked on — came out at the EDITOR DEFAULT density while
//  the face it grows out of was at some other one.  The seam between them read as a
//  texture change, which is exactly what "doesn't look natural with the parent
//  solid edge area" describes.
//
//  So: scaleS = srcTex->size[0] / texWidth reproduces the face's own texel density on
//  the patch, in the patch's own arc-length parametrisation.  Both are read from
//  the SAME material (the patch has already inherited the face's), so the widths
//  cancel and the two surfaces agree by construction rather than by luck.
//
//  Patch_KiwiFinishNew is unchanged in behaviour — it is now a one-line forwarder
//  passing a null texdef, which takes the identical default path.  No ported caller
//  is touched.
// ════════════════════════════════════════════════════════════════════════════
void Patch_KiwiFinishNewLike( patchMesh_t *p, const texdef_sub_t *srcTex )
{
    if ( !p )
        return;

    float natScale = g_qeglobals.random_texture_stuff[0].sampleSize;
    if ( natScale == 0.0f )
        natScale = 0.25f;
    float sScale = natScale;
    float tScale = natScale;
    if ( srcTex )
    {
        // The SAME material the patch is about to be textured with — layer 0, the
        // channel Patch_Naturalize2 is called on below.
        qtexture_s *lm = MaterialDef_GetLayeredMaterial( (MaterialDef *)&p->texture );
        const int w = lm ? lm->width  : 512;
        const int h = lm ? lm->height : 512;
        if ( srcTex->size[0] > 0.0f && w > 0 ) sScale = srcTex->size[0] / (float)w;
        if ( srcTex->size[1] > 0.0f && h > 0 ) tScale = srcTex->size[1] / (float)h;
        if ( !( sScale > 0.0f ) ) sScale = natScale;
        if ( !( tScale > 0.0f ) ) tScale = natScale;
    }
    Patch_Naturalize2( p, 0, sScale, tScale );

    const bool terrain = ( p->type & PATCH_TERRAIN ) != 0;
    if ( terrain ) Patch_TerrainTexProject( p, 1, 16.0f );
    else           Patch_ResampleCurveST( p, 1, 16.0f );
    *(float *)&p->size_of_struct_0x504C = 16.0f;
    p->bDirty = 1;                       // gates `size` in Patch_Write (:1146)
    if ( terrain ) Patch_TerrainTexProject( p, 2, 0.25f );
    else           Patch_ResampleCurveST( p, 2, 0.25f );

    if ( p->curveDef )
        free( p->curveDef );
    p->curveDef = Patch_GenericMesh2( p, g_qeglobals.current_edit_layer, 0, 0 );
    ++p->version;
}

// The ROUND Q entry point, unchanged in behaviour: a null texdef takes the
// editor-default `random_texture_stuff[0].sampleSize` path exactly as before.
void Patch_KiwiFinishNew( patchMesh_t *p )
{
    Patch_KiwiFinishNewLike( p, 0 );
}

// ════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND AK, ITEM 3) — Patch_KiwiReNaturalize
//
//  USER REPORT, verbatim: "bevel is good, but I can't select the curve parts to
//  retexture them.  See how it's messed up?"  The screenshot's second half — the
//  stretched texture on the fillet — is a SEPARATE, CERTAIN defect from the
//  selectability chain (fixed in kiwi_pick.cpp), and this is it.
//
//  WHAT THE PORTED APPLY DOES, AND WHY IT STRETCHES.  Texture_SetTexture reaches a
//  patch through Brush_SetAllFacesMaterial (brush.cpp) with materialOnly, whose patch branch
//  copies the two material POINTERS (lyrMtl, radMtl) and bumps p->version.  It
//  never touches ctrl[][].texCoord and it never naturalizes — faithful to 0x476ED0
//  and NOT to be changed there.  So the per-control-point S/T stay exactly as
//  Patch_Naturalize2 laid them against the OLD material's width/height, and the
//  world-units-per-repeat silently rescales by newWidth/oldWidth.  Swap a 512 px
//  wall material for a 128 px one and the patch comes out four times denser.  On a
//  fillet — whose S runs along the arc at the parent wall's inherited density
//  (Patch_KiwiFinishNewLike above) — that reads as exactly the reported stretch.
//
//  THE COMPOSITION RULE, AND IT IS RECOVERABLE WITHOUT CAPTURING ANYTHING FIRST.
//  Patch_Naturalize2 lays a LINEAR map: st[0]_i = widthDist(i) / (w * scaleS), and
//  `w * scaleS` IS the world units one repeat covers.  Since the apply does not touch
//  ctrl[][], the OLD product survives the swap in the coordinates themselves:
//
//      WUPR_s = widthDist ( width  - 1 ) /  ctrl[width -1][0].texCoord.st[0]
//      WUPR_t = heightDist( height - 1 ) / -ctrl[0][height-1].texCoord.st[1]
//
//  (the T sign comes from Patch_Naturalize2's negated tScale, :1233).  Feeding
//  those back as a synthesized texdef_sub_t.size[] makes Patch_KiwiFinishNewLike
//  compute scaleS = WUPR_s / newWidth, and Patch_Naturalize2 then re-lays at
//  1 / (newWidth * WUPR_s / newWidth) = 1 / WUPR_s — the density the patch already
//  had, against the material it now has.  The old width cancels out, so nothing has
//  to be captured before the apply runs.
//
//  ROUND AJ'S INHERITANCE AND A MANUAL RETEXTURE THEREFORE COMPOSE: the fillet is
//  born at its parent face's density, and re-texturing it KEEPS that density
//  instead of resetting to the editor default.  A patch whose coordinates are
//  degenerate (a zero span, or an S that never advanced) falls back to
//  Patch_KiwiFinishNew's editor default, which is what stock Patch→Naturalize does
//  and is always better than leaving the stretch in place.
//
//  It also rebuilds curveDef (Patch_KiwiFinishNewLike's tail), which is what
//  Patch_RayTest's pick needs — so this is self-healing on that axis too.
static void Patch_KiwiReNaturalize( patchMesh_t *p )
{
    if ( !p || p->width < 2 || p->height < 2 )
        return;

    const float sEnd = p->ctrl[p->width  - 1][0].texCoord.st[0];
    const float tEnd = p->ctrl[0][p->height - 1].texCoord.st[1];
    const float wDist = (float)Patch_WidthDistanceTo ( p->width  - 1, p );
    const float hDist = (float)Patch_HeightDistanceTo( p->height - 1, p );

    texdef_sub_t td;
    memset( &td, 0, sizeof( td ) );
    bool have = false;
    if ( wDist > 0.0f && sEnd > 1.0e-6f )
    {
        td.size[0] = wDist / sEnd;
        have = true;
    }
    if ( hDist > 0.0f && tEnd < -1.0e-6f )
    {
        td.size[1] = hDist / -tEnd;
        have = true;
    }
    // A zero size[] component is the "no bound" answer Patch_KiwiFinishNewLike
    // already documents (`srcTex->size[0] > 0.0f` gates each axis independently),
    // so a patch with only one recoverable axis keeps that one and takes the
    // editor default on the other.
    Patch_KiwiFinishNewLike( p, have ? &td : 0 );
}

// The selection sweep.  Called from the KIWI texture-apply fence (texwnd.cpp)
// immediately after Texture_SetTexture, because that is the one funnel a thumbnail
// click goes through and it is above every ported apply path.  Walks the SAME
// sentinel list Brush_SetTexture's brush pass walks, so anything the apply reached
// is exactly what this re-lays and nothing else.
void Patch_KiwiReNaturalizeSelected()
{
    // Sentinel walk: init from .next, advance via ->next.
    for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
    {
        if ( !b->patch || !b->def )
            continue;
        Patch_KiwiReNaturalize( b->def->patch );
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  Patch_GenericMesh  (0x43b310)
//
//  The "Simple Patch Mesh" backend (Curve→Simple Patch Mesh, cmd 32856 → the
//  CPatchDensityDlg width/height-density dialog → CPatchDensityDlg_OnOK 0x436390).
//  Builds a FLAT nWidth×nHeight Bezier patch laid over the selected single brush's
//  bounding box, in the plane of the active XY/XZ/YZ view (nOrientation =
//  ActiveXY->m_nViewType), stamps the current-layer materials, naturalizes the
//  texcoords, builds the render curveDef, links the new patch brush into the active
//  list, and (bDeleteSource) replaces the source selection with it.
//
//  Notes:
//   * nWidth/nHeight bounds: nHeight>15 || (unsigned)(nWidth-3)>12 → reject (3..15).
//   * Orientation→axis map: the (u,v) plane axes for the view:
//       view 0 → (u=1,v=2)   view 1 → (u=0,v=2)   view 2 → (u=0,v=1)
//     and the patch is flattened at def->maxs[nOrientation] on the normal axis.
//   * u/v coords are INT-truncated walks of def->mins[axis] stepped by the
//     fabs()'d axis span / (count-1) (the int truncations and double-precision
//     step adds are the binary's; preserved).
//   * Texture/lightmap layers come from g_qeglobals.random_texture_stuff (the
//     current-edit-layer templates), NOT the source brush's faces.
//   * Naturalize scale = random_texture_stuff[0].sampleSize; falls back to the
//     editor's 0.25 default when unseeded (headless gate-safety, the same guard as
//     Patch_BrushToMesh — identical when the GUI is up).
//   * The bDirty texCoord refinement (Patch_TerrainTexProject terrain / Patch_ResampleCurveST
//     curve, both at 16.0 then 0.25, layers 1/2) runs.
//   * type is 0 — the `type & PATCH_TERRAIN` forks below are therefore DEAD in the
//     binary too; they are kept because the binary emits them.
//
//  Terrain creation is the separate Create_Terrain function. It uses
//  random_texture_stuff; it is not a PATCH_TERRAIN variant of this function and does
//  not project materials from the selected brush face.
// ════════════════════════════════════════════════════════════════════════════
selbrush_t *Patch_GenericMesh( int nWidth, int nHeight, int nOrientation,
                               char bDeleteSource, char bOverwrite )
{
    if ( nHeight > 15 || (unsigned int)( nWidth - 3 ) > 12 )
    {
        Sys_Printf( "Invalid patch width or height.\n" );
        return nullptr;
    }
    if ( !bOverwrite && !QE_SingleBrush() )
    {
        Sys_Printf( "Cannot generate a patch from multiple selections.\n" );
        return nullptr;
    }

    patchMesh_t *p = MakeNewPatch();
    p->height = nHeight;
    p->width  = nWidth;
    p->type   = (PATCH_TYPES)0;

    // Orientation → (u-axis, v-axis) of the view plane.
    int uAxis, vAxis;
    if ( nOrientation == 0 )       { uAxis = 1; vAxis = 2; }
    else if ( nOrientation == 1 )  { uAxis = 0; vAxis = 2; }
    else                           { uAxis = 0; vAxis = 1; }

    brush_t  *def   = selected_brushes.next->def;
    entity_s *owner = selected_brushes.next->owner;

    // |span| / (count-1) step on each plane axis (fabs'd, double-precision divides).
    float uStep = (float)fabs( (double)( def->maxs[uAxis] - def->mins[uAxis] ) / (double)( nWidth  - 1 ) );
    float vStep = (float)fabs( (double)( def->maxs[vAxis] - def->mins[vAxis] ) / (double)( nHeight - 1 ) );

    int u = (int)def->mins[uAxis];
    for ( int i = 0; i < nWidth; ++i )
    {
        int   v  = (int)def->mins[vAxis];
        float uf = (float)u;
        for ( int j = 0; j < nHeight; ++j )
        {
            float vf = (float)v;
            p->ctrl[i][j].xyz[uAxis]        = uf;
            p->ctrl[i][j].xyz[vAxis]        = vf;
            p->ctrl[i][j].xyz[nOrientation] = def->maxs[nOrientation];
            v = (int)( vf + vStep );
        }
        u = (int)( (double)u + uStep );
    }

    // Stamp the current-edit-layer templates: layer 0 = texture, layer 1 = lightmap.
    // smoothing keeps MakeNewPatch's "smoothing_smooth".
    p->texture.lyrMtl  = g_qeglobals.random_texture_stuff[0].mtl.lyrMtl;
    p->texture.radMtl  = g_qeglobals.random_texture_stuff[0].mtl.radMtl;
    p->lightmap.lyrMtl = g_qeglobals.random_texture_stuff[1].mtl.lyrMtl;
    p->lightmap.radMtl = g_qeglobals.random_texture_stuff[1].mtl.radMtl;

    float natScale = g_qeglobals.random_texture_stuff[0].sampleSize;
    if ( natScale == 0.0f )
        natScale = 0.25f;
    Patch_Naturalize2( p, 0, natScale, natScale );

    const bool terrain = ( p->type & PATCH_TERRAIN ) != 0;       // always false
    if ( terrain ) Patch_TerrainTexProject( p, 1, 16.0f );
    else           Patch_ResampleCurveST( p, 1, 16.0f );
    *(float *)&p->size_of_struct_0x504C = 16.0f;
    p->bDirty = 1;
    if ( terrain ) Patch_TerrainTexProject( p, 2, 0.25f );
    else           Patch_ResampleCurveST( p, 2, 0.25f );

    if ( p->curveDef )
        free( p->curveDef );
    p->curveDef = Patch_GenericMesh2( p, g_qeglobals.current_edit_layer, 0, 0 );
    ++p->version;

    brush_t   *pdef = AddBrushForPatch( p, (entity_s *)owner->def );
    selbrush_t *inst = Brush_AddToList( pdef, owner );
    if ( inst->next || inst->prev )
        Error( "Brush_AddToList: already linked" );
    if ( &active_brushes == &selected_brushes )
        Brush_AddToList2( inst );
    else
    {
        inst->next = active_brushes.next;
        active_brushes.next->prev = inst;
        active_brushes.next = inst;
        inst->prev = &active_brushes;
    }

    if ( bDeleteSource )
    {
        Select_Delete();
        Select_Brush( inst, 1, 1, 0 );
    }

    return inst;
}

// ── Patch_BuildCurveDef (0x438D50) — free + rebuild the tessellated render mesh ──
//    The named helper the binary calls where Patch_BrushToMesh / Patch_GenericMesh
//    inline the same three statements.
static curvePatchDef_t *Patch_BuildCurveDef( patchMesh_t *p )
{
    if ( p->curveDef )
        free( p->curveDef );
    curvePatchDef_t *result = Patch_GenericMesh2( p, g_qeglobals.current_edit_layer, 0, 0 );
    p->curveDef = result;
    return result;
}

// Create_Terrain (0x43B660), called by TerrainDlg_NewPatch (0x458FB0).
selbrush_t *Create_Terrain( int width, int height, int orientation )
{
    if ( (unsigned int)( height - 2 ) > 14 || (unsigned int)( width - 2 ) > 14 )
    {
        Sys_Printf( "Invalid terrain width or height.\n" );
        return nullptr;
    }
    if ( !QE_SingleBrush() )
    {
        Sys_Printf( "Cannot generate a terrain from multiple selections.\n" );
        return nullptr;
    }

    patchMesh_t *p = MakeNewPatch();
    p->height = height;
    p->width  = width;
    p->type   = PATCH_TERRAIN;

    int uAxis, vAxis;
    if ( orientation == 0 )       { uAxis = 1; vAxis = 2; }
    else if ( orientation == 1 )  { uAxis = 0; vAxis = 2; }
    else                          { uAxis = 0; vAxis = 1; }

    brush_t  *def   = selected_brushes.next->def;
    entity_s *owner = selected_brushes.next->owner;

    float uStep = (float)fabs( (double)( def->maxs[uAxis] - def->mins[uAxis] ) / (double)( width  - 1 ) );
    float vStep = (float)fabs( (double)( def->maxs[vAxis] - def->mins[vAxis] ) / (double)( height - 1 ) );

    int u = (int)def->mins[uAxis];
    for ( int i = 0; i < width; ++i )
    {
        int   v  = (int)def->mins[vAxis];
        float uf = (float)u;
        for ( int j = 0; j < height; ++j )
        {
            float vf = (float)v;
            p->ctrl[i][j].xyz[uAxis]      = uf;
            p->ctrl[i][j].xyz[vAxis]      = vf;
            p->ctrl[i][j].xyz[orientation] = def->maxs[orientation];
            v = (int)( vf + vStep );
        }
        u = (int)( (double)u + uStep );
    }

    p->texture.lyrMtl  = g_qeglobals.random_texture_stuff[0].mtl.lyrMtl;
    p->texture.radMtl  = g_qeglobals.random_texture_stuff[0].mtl.radMtl;
    p->lightmap.lyrMtl = g_qeglobals.random_texture_stuff[1].mtl.lyrMtl;
    p->lightmap.radMtl = g_qeglobals.random_texture_stuff[1].mtl.radMtl;

    const float natScale = g_qeglobals.random_texture_stuff[0].sampleSize;
    Patch_Naturalize2( p, 0, natScale, natScale );

    const bool terrain = ( p->type & PATCH_TERRAIN ) != 0;                         // always true here
    if ( terrain ) Patch_TerrainTexProject( p, 1, 16.0f );
    else           Patch_ResampleCurveST( p, 1, 16.0f );
    *(float *)&p->size_of_struct_0x504C = 16.0f;
    p->bDirty = 1;
    if ( terrain ) Patch_TerrainTexProject( p, 2, 0.25f );
    else           Patch_ResampleCurveST( p, 2, 0.25f );

    Patch_BuildCurveDef( p );
    ++p->version;

    brush_t    *pdef = AddBrushForPatch( p, (entity_s *)owner->def );
    selbrush_t *inst = Brush_AddToList( pdef, owner );
    // Select_Brush_2(&active_brushes, inst) (0x476630), inlined — list != selected_brushes
    if ( inst->next || inst->prev )
        Error( "Brush_AddToList: already linked" );
    if ( &active_brushes == &selected_brushes )
        Brush_AddToList2( inst );
    else
    {
        inst->next = active_brushes.next;
        active_brushes.next->prev = inst;
        active_brushes.next = inst;
        inst->prev = &active_brushes;
    }

    Select_Delete();
    Select_Brush( inst, 1, 1, 0 );
    return inst;
}

// ════════════════════════════════════════════════════════════════════════════
//  GRID MANIPULATION — insert / remove a column or row of control points.
//
//  Each function has a TERRAIN branch (type & PATCH_TERRAIN) that adds/removes a
//  SINGLE row/col via the 4 helpers below, and a BEZIER branch that adds/removes
//  TWO (keeping the odd 2n+1 grid).  CoD divergences vs GtkRadiant:
//   * the new flanking points are offset by ±1/3 of the adjacent segment delta,
//     NOT GtkRadiant's ±1/2;
//   * the insert/remove POSITION is chosen by the `flag` arg (front vs back), NOT
//     GtkRadiant's PointInMoveList vertex-selection scan;
//   * REMOVE is a plain 2-column/row drop (no GtkRadiant extrapolation);
//   * the tail refreshes CWnd_PatchDialog instead of Patch_EditPatch (Patch_AdjustSelected
//     has no refresh tail — the refresh belongs to the callers).
// ════════════════════════════════════════════════════════════════════════════

// ─── terrain single-row/col helpers ──────────────────────────────────────────
// 0x4436A0 — insert ONE row at index `at` (height += 1), midpoint-averaged.
static void Patch_TerrainInsertRow( patchMesh_t *p, int at )
{
    if ( p->height + 1 > 16 )
        return;
    for ( int col = 0; col < p->width; ++col )
    {
        for ( int row = p->height - 1; row >= at; --row )
            p->ctrl[col][row + 1] = p->ctrl[col][row];
        // new row `at` = average of the (now shifted) neighbours, xyz + st0/st1 + color
        drawVert_t *nv = &p->ctrl[col][at];
        drawVert_t *lo = &p->ctrl[col][at - 1];
        drawVert_t *hi = &p->ctrl[col][at + 1];
        for ( int k = 0; k < 3; ++k )
            nv->xyz[k] = ( lo->xyz[k] + hi->xyz[k] ) * 0.5f;
        for ( int k = 0; k < 2; ++k )
        {
            nv->texCoord.st[k]        = ( lo->texCoord.st[k]        + hi->texCoord.st[k] )        * 0.5f;
            nv->texCoord.lightmap[k]  = ( lo->texCoord.lightmap[k]  + hi->texCoord.lightmap[k] )  * 0.5f;
            nv->texCoord.smoothing[k] = ( lo->texCoord.smoothing[k] + hi->texCoord.smoothing[k] ) * 0.5f;
        }
        ((byte *)&nv->vert_color)[0] = (byte)( ( ((byte*)&lo->vert_color)[0] + ((byte*)&hi->vert_color)[0] ) / 2 );
        ((byte *)&nv->vert_color)[1] = (byte)( ( ((byte*)&lo->vert_color)[1] + ((byte*)&hi->vert_color)[1] ) / 2 );
        ((byte *)&nv->vert_color)[2] = (byte)( ( ((byte*)&lo->vert_color)[2] + ((byte*)&hi->vert_color)[2] ) / 2 );
        ((byte *)&nv->vert_color)[3] = (byte)( ( ((byte*)&lo->vert_color)[3] + ((byte*)&hi->vert_color)[3] ) / 2 );
        // register the new control point as an editable handle (d_points append).
        for ( int k = 0; k < 3; ++k )
            g_qeglobals.d_points[g_qeglobals.d_numpoints][k] = nv->xyz[k];
        if ( g_qeglobals.d_numpoints < 2047 )
            ++g_qeglobals.d_numpoints;
    }
    ++p->height;
}

// 0x443260 — insert ONE column at index `at` (width += 1), midpoint-averaged.
static void Patch_TerrainInsertColumn( patchMesh_t *p, int at )
{
    if ( p->width + 1 > 16 )
        return;
    for ( int row = 0; row < p->height; ++row )
    {
        for ( int col = p->width - 1; col >= at; --col )
            p->ctrl[col + 1][row] = p->ctrl[col][row];
        drawVert_t *nv = &p->ctrl[at][row];
        drawVert_t *lo = &p->ctrl[at - 1][row];
        drawVert_t *hi = &p->ctrl[at + 1][row];
        for ( int k = 0; k < 3; ++k )
            nv->xyz[k] = ( lo->xyz[k] + hi->xyz[k] ) * 0.5f;
        for ( int k = 0; k < 2; ++k )
        {
            nv->texCoord.st[k]        = ( lo->texCoord.st[k]        + hi->texCoord.st[k] )        * 0.5f;
            nv->texCoord.lightmap[k]  = ( lo->texCoord.lightmap[k]  + hi->texCoord.lightmap[k] )  * 0.5f;
            nv->texCoord.smoothing[k] = ( lo->texCoord.smoothing[k] + hi->texCoord.smoothing[k] ) * 0.5f;
        }
        ((byte *)&nv->vert_color)[0] = (byte)( ( ((byte*)&lo->vert_color)[0] + ((byte*)&hi->vert_color)[0] ) / 2 );
        ((byte *)&nv->vert_color)[1] = (byte)( ( ((byte*)&lo->vert_color)[1] + ((byte*)&hi->vert_color)[1] ) / 2 );
        ((byte *)&nv->vert_color)[2] = (byte)( ( ((byte*)&lo->vert_color)[2] + ((byte*)&hi->vert_color)[2] ) / 2 );
        ((byte *)&nv->vert_color)[3] = (byte)( ( ((byte*)&lo->vert_color)[3] + ((byte*)&hi->vert_color)[3] ) / 2 );
        // register the new control point as an editable handle (d_points append).
        for ( int k = 0; k < 3; ++k )
            g_qeglobals.d_points[g_qeglobals.d_numpoints][k] = nv->xyz[k];
        if ( g_qeglobals.d_numpoints < 2047 )
            ++g_qeglobals.d_numpoints;
    }
    ++p->width;
}

// 0x443AE0 — remove ONE row at index `at` (height -= 1, min 3), shift rows down.
static void Patch_TerrainRemoveRow( patchMesh_t *p, int at )
{
    if ( p->height <= 2 )
        return;
    --p->height;
    if ( at == p->height )
        return;                               // removed the last row → nothing to shift
    for ( int col = 0; col < p->width; ++col )
        for ( int row = at; row < p->height; ++row )
            p->ctrl[col][row] = p->ctrl[col][row + 1];
}

// 0x443C10 — remove ONE column at index `at` (width -= 1, min 3), shift cols left.
static void Patch_TerrainRemoveColumn( patchMesh_t *p, int at )
{
    if ( p->width <= 2 )
        return;
    --p->width;
    if ( at == p->width )
        return;
    for ( int col = at; col < p->width; ++col )
        for ( int row = 0; row < p->height; ++row )
            p->ctrl[col][row] = p->ctrl[col + 1][row];
}

// ─── Patch_InsertColumn (0x443410) ───────────────────────────────────────────
void Patch_InsertColumn( patchMesh_t *p, char flag )
{
    if ( ( p->type & PATCH_TERRAIN ) != 0 )
    {
        Patch_TerrainInsertColumn( p, flag ? ( p->width - 1 ) : 1 );
        return;
    }
    if ( p->width + 2 > 16 )
        return;

    if ( flag )
    {
        // insert 2 columns at the END: subdivide the last segment into thirds.
        int w = p->width;                     // old column count
        for ( int h = 0; h < p->height; ++h )
        {
            float third[3];
            for ( int k = 0; k < 3; ++k )
                third[k] = ( p->ctrl[w - 1][h].xyz[k] - p->ctrl[w - 2][h].xyz[k] ) * (float)kOneThird;
            p->ctrl[w + 1][h] = p->ctrl[w - 1][h];                 // old last → new endpoint
            p->ctrl[w - 1][h] = p->ctrl[w - 2][h];                 // new col w-1 base
            for ( int k = 0; k < 3; ++k ) p->ctrl[w - 1][h].xyz[k] += third[k];
            p->ctrl[w][h] = p->ctrl[w - 1][h];                     // new col w base
            for ( int k = 0; k < 3; ++k ) p->ctrl[w][h].xyz[k] += third[k];
        }
    }
    else
    {
        // insert 2 columns at the FRONT (after col 0): shift up, build cols 1 & 2.
        for ( int h = 0; h < p->height; ++h )
        {
            // the shift includes col 0: ctrl[col+2]=ctrl[col] for col=width-1..0, so ctrl[2]
            // inherits col-0's full vertex.
            for ( int col = p->width - 1; col >= 0; --col )
                p->ctrl[col + 2][h] = p->ctrl[col][h];
            float third[3];
            for ( int k = 0; k < 3; ++k )
                third[k] = ( p->ctrl[3][h].xyz[k] - p->ctrl[0][h].xyz[k] ) * (float)kOneThird;
            // ONLY the 3 xyz floats of the two new columns are written: ctrl[1] keeps ORIGINAL
            // col-1's texCoord/normal/color (never a shift dest), ctrl[2] keeps col-0's (from
            // the inclusive shift above).  Full-struct copies would clobber ctrl[1]'s non-xyz
            // fields with col-0's.
            for ( int k = 0; k < 3; ++k ) p->ctrl[1][h].xyz[k] = p->ctrl[0][h].xyz[k] + third[k];
            for ( int k = 0; k < 3; ++k ) p->ctrl[2][h].xyz[k] = p->ctrl[1][h].xyz[k] + third[k];
        }
    }
    p->width += 2;
}

// ─── Patch_InsertRow (0x443830) ──────────────────────────────────────────────
void Patch_InsertRow( patchMesh_t *p, char flag )
{
    if ( ( p->type & PATCH_TERRAIN ) != 0 )
    {
        if ( p->height + 1 > 16 )
            return;
        Patch_TerrainInsertRow( p, flag ? ( p->height - 1 ) : 1 );
        return;
    }
    if ( p->height + 2 > 16 )
        return;

    if ( flag )
    {
        int h = p->height;                    // old row count
        for ( int w = 0; w < p->width; ++w )
        {
            float third[3];
            // InsertRow uses `delta / 3.0`, NOT InsertColumn's `* 0.3333333432674408` —
            // keep the exact constant.
            for ( int k = 0; k < 3; ++k )
                third[k] = (float)( (double)( p->ctrl[w][h - 1].xyz[k] - p->ctrl[w][h - 2].xyz[k] ) / 3.0 );
            p->ctrl[w][h + 1] = p->ctrl[w][h - 1];
            p->ctrl[w][h - 1] = p->ctrl[w][h - 2];
            for ( int k = 0; k < 3; ++k ) p->ctrl[w][h - 1].xyz[k] += third[k];
            p->ctrl[w][h] = p->ctrl[w][h - 1];
            for ( int k = 0; k < 3; ++k ) p->ctrl[w][h].xyz[k] += third[k];
        }
    }
    else
    {
        for ( int w = 0; w < p->width; ++w )
        {
            for ( int row = p->height - 1; row >= 1; --row )
                p->ctrl[w][row + 2] = p->ctrl[w][row];
            float third[3];
            // `delta / 3.0`, not `* kOneThird`.
            for ( int k = 0; k < 3; ++k )
                third[k] = (float)( (double)( p->ctrl[w][3].xyz[k] - p->ctrl[w][0].xyz[k] ) / 3.0 );
            // ONLY xyz of the two new rows is written: ctrl[w][1] keeps ORIGINAL row-1's non-xyz,
            // ctrl[w][2] keeps row-2's — the row shift is row>=1 (ctrl[w][2] is never a dest),
            // unlike InsertColumn's col0-inclusive shift.
            for ( int k = 0; k < 3; ++k ) p->ctrl[w][1].xyz[k] = p->ctrl[w][0].xyz[k] + third[k];
            for ( int k = 0; k < 3; ++k ) p->ctrl[w][2].xyz[k] = p->ctrl[w][1].xyz[k] + third[k];
        }
    }
    p->height += 2;
}

// ─── Patch_RemoveColumn (0x443C90) ───────────────────────────────────────────
void Patch_RemoveColumn( patchMesh_t *p, char flag )
{
    if ( ( p->type & PATCH_TERRAIN ) != 0 )
    {
        Patch_TerrainRemoveColumn( p, flag ? 0 : ( p->width - 1 ) );
        return;
    }
    if ( p->width <= 3 )
        return;
    p->width -= 2;
    if ( flag )                               // remove the FIRST 2 columns: shift left by 2
    {
        for ( int row = 0; row < p->height; ++row )
            for ( int col = 0; col < p->width; ++col )
                p->ctrl[col][row] = p->ctrl[col + 2][row];
    }
    // flag clear: drop the LAST 2 columns (no shift; the width-- already did it).
}

// ─── Patch_RemoveRow (0x443B60) ──────────────────────────────────────────────
void Patch_RemoveRow( patchMesh_t *p, char flag )
{
    if ( ( p->type & PATCH_TERRAIN ) != 0 )
    {
        Patch_TerrainRemoveRow( p, flag ? 0 : ( p->height - 1 ) );
        return;
    }
    if ( p->height <= 3 )
        return;
    p->height -= 2;
    if ( flag )                               // remove the FIRST 2 rows: shift down by 2
    {
        for ( int col = 0; col < p->width; ++col )
            for ( int row = 0; row < p->height; ++row )
                p->ctrl[col][row] = p->ctrl[col][row + 2];
    }
    // flag clear: drop the LAST 2 rows.
}

// ─── Patch_Rebuild (0x438D80) — texCoord refine + bbox + curveDef rebuild ─────
//    Refines texCoords (Patch_ResampleCurveST/Patch_TerrainTexProject) when bDirty, then
//    rebuilds the symbiont bbox (Brush_RebuildBrush) and the curveDef (Patch_GenericMesh2).
//    Without the refinement, every grid edit that routes through Patch_Rebuild
//    (insert/remove row+column via Patch_AdjustSelected, OnDropSelectedRelativeZ)
//    would leave the patch's S/T untouched, so edited patches would keep pre-edit texturing.
void Patch_Rebuild( patchMesh_t *p, char doBounds )   // non-static: OnDropSelectedRelativeZ calls it
{
    if ( doBounds )
    {
        if ( p->bDirty )
        {
            // sampleSize is SAVED, passed to the refine, then written BACK afterwards
            // along with bDirty=1 — the refine may clobber both, and the binary
            // deliberately preserves size / re-dirties. Layer is 1 for both branches.
            float sampleSize = *(float *)&p->size_of_struct_0x504C;
            if ( ( p->type & PATCH_TERRAIN ) != 0 )
                Patch_TerrainTexProject( p, 1, sampleSize );
            else
                Patch_ResampleCurveST( p, 1, sampleSize );
            p->bDirty = 1;
            *(float *)&p->size_of_struct_0x504C = sampleSize;
        }
        float mins[3], maxs[3];
        Patch_CalcBounds( mins, maxs, p, true );
        Brush_RebuildBrush( (brush_t *)p->pSymbiot, mins, maxs );
    }
    if ( p->curveDef )
        free( p->curveDef );
    // rebuild the tessellated render mesh so grid edits re-render as wireframe.
    p->curveDef = Patch_GenericMesh2( p, g_qeglobals.current_edit_layer, 0, 0 );
    ++p->version;
}

// KIWI: Patch_RebuildAllCurveDefs (0x4495C0, OnPrefs's re-tessellation of every active patch)
// is gone - no preference feeds the tessellation, and the texture-layer switch re-tessellates
// through Material_SetMode -> BrushList_RebuildDisplay.

// ─── Patch_AdjustSelected (0x444550) — apply insert/remove to every selected patch ─
void Patch_AdjustSelected( char bInsert, char bColumn, char bFlag )
{
    char bUpdate = 0;
    for ( selbrush_t *b = selected_brushes.next;
          b != &selected_brushes;
          b = b->next )
    {
        if ( !b->patch )
            continue;
        patchMesh_t *p = b->def->patch;       // the patch DEF (instance.patch->def == def->patch)
        iassert( b->patch->def == b->def->patch );   // PMESH.CPP:5316
        if ( !p )
            continue;                         // defensive: never-built patch (binary asserts above)
        if ( bInsert )
        {
            if ( bColumn ) Patch_InsertColumn( p, bFlag );
            else           Patch_InsertRow( p, bFlag );
        }
        else
        {
            if ( bColumn ) Patch_RemoveColumn( p, bFlag );
            else           Patch_RemoveRow( p, bFlag );
        }
        bUpdate = 1;
        // post-op stability assert (PMESH.CPP:5333) — def didn't realloc.
        iassert( p == b->patch->def );
        Patch_Rebuild( p, 1 );
    }
    if ( bUpdate )
        g_nUpdateBits = -1;
}

// ─── patchInvert2 (0x446480) — VERTICAL FLIP: mirror each column's rows ────────
//   Swaps ctrl[col][r] <-> ctrl[col][height-1-r] for r in [0, height/2).  Terrain
//   patches then propagate each control vertex's turned_edge bit0 up one row over rows
//   0..height-2.
static void patchInvert2( patchMesh_t *p )
{
    for ( int col = 0; col < p->width; ++col )
    {
        for ( int r = 0; r < p->height / 2; ++r )
        {
            drawVert_t tmp                  = p->ctrl[col][r];
            p->ctrl[col][r]                 = p->ctrl[col][p->height - 1 - r];
            p->ctrl[col][p->height - 1 - r] = tmp;
        }
        if ( ( p->type & PATCH_TERRAIN ) != 0 )
        {
            for ( int r = 0; r < p->height - 1; ++r )
                p->ctrl[col][r].turned_edge ^=
                    ( p->ctrl[col][r].turned_edge ^ p->ctrl[col][r + 1].turned_edge ) & 1;
        }
    }
}

// ─── Patch_ToggleInverted (0x4465c0) — Curve→Negative: vertical-flip selection ─
//   For every selected patch: vertical-flip its control grid (patchInvert2) and
//   rebuild the tessellated render mesh (free + Patch_GenericMesh2, ++version).
//   NO bbox-rebuild here (a vertical flip leaves the bounds unchanged), so — unlike
//   Patch_Transpose — there is no Patch_Rebuild/Brush_RebuildBrush call.  Ends with the
//   patch-inspector dialog refresh (g_PatchDialog_GetPatchInfo).
void Patch_ToggleInverted()
{
    char bUpdate = 0;
    for ( selbrush_t *b = selected_brushes.next;
          b != &selected_brushes;
          b = b->next )
    {
        if ( !b->patch )
            continue;
        // patch instance's first member is its patchMesh_t* def (patch_t::def);
        // == b->def->patch by the invariant below (cf. brush.cpp:5278 idiom).
        patchMesh_t *def = b->patch->def;
        iassert( b->patch->def == b->def->patch );
        patchInvert2( def );
        if ( def->curveDef )
            free( def->curveDef );
        def->curveDef = Patch_GenericMesh2( def, g_qeglobals.current_edit_layer, 0, 0 );
        bUpdate = 1;
        ++def->version;
    }
    if ( bUpdate )
        g_nUpdateBits = -1;

    // refresh the patch-inspector dialog (unconditional tail, outside the selection block); a
    // no-op headless.  Patch_Move has the identical tail.
    if ( PatchDialog_IsOpen() )
        g_PatchDialog_GetPatchInfo();
}

// ─── Patch_TransposeGrid (0x449020) — in-place control-grid TRANSPOSE ──
//   The matrix-transpose half of Patch_Transpose: reflects ctrl[c][r] across the
//   diagonal (ctrl[i][j] <-> ctrl[j][i]), then swaps width<->height.  The reflection
//   is split into two orientations (width<=height vs width>height) so the
//   rectangular cells whose mirror lies OUTSIDE the original grid are COPIED into
//   their soon-to-be-valid slot rather than swapped.  Terrain patches first toggle
//   every control vertex's turned_edge bit0.
static patchMesh_t *Patch_TransposeGrid( patchMesh_t *p )
{
    if ( ( p->type & PATCH_TERRAIN ) != 0 )
        for ( int c = 0; c < p->width; ++c )
            for ( int r = 0; r < p->height; ++r )
                p->ctrl[c][r].turned_edge ^= 1u;

    if ( p->width <= p->height )
    {
        for ( int c = 0; c < p->width; ++c )
            for ( int r = c + 1; r < p->height; ++r )
            {
                if ( r < p->width )                       // mirror in-grid: swap
                {
                    drawVert_t tmp = p->ctrl[c][r];
                    p->ctrl[c][r]  = p->ctrl[r][c];
                    p->ctrl[r][c]  = tmp;
                }
                else                                      // mirror off-grid: copy up
                    p->ctrl[r][c] = p->ctrl[c][r];
            }
    }
    else
    {
        for ( int c = 0; c < p->height; ++c )
            for ( int r = c + 1; r < p->width; ++r )
            {
                if ( r < p->height )                      // mirror in-grid: swap
                {
                    drawVert_t tmp = p->ctrl[c][r];
                    p->ctrl[c][r]  = p->ctrl[r][c];
                    p->ctrl[r][c]  = tmp;
                }
                else                                      // mirror off-grid: copy up
                    p->ctrl[c][r] = p->ctrl[r][c];
            }
    }

    int t     = p->height;
    p->height = p->width;
    p->width  = t;
    return p;
}

// ─── Patch_Transpose (0x4491D0) — Curve→Matrix Transpose: transpose selected patches ─
//   For every selected patch: transpose the control grid (Patch_TransposeGrid) THEN
//   vertical-flip it (patchInvert2 — the binary pairs the two), rebuild the tessellated
//   curveDef (free + Patch_GenericMesh2, ++version), then Patch_Rebuild(def,1) (bbox +
//   curveDef rebuild — a transpose changes the bounds, unlike a pure flip).  Both the
//   inline curveDef rebuild AND Patch_Rebuild run (so version bumps twice and the mesh
//   builds twice), as in the binary.  g_nUpdateBits is set by the OnCurveMatrixTranspose
//   handler, not here.
void Patch_Transpose()
{
    for ( selbrush_t *b = selected_brushes.next;
          b != &selected_brushes;
          b = b->next )
    {
        if ( !b->patch )
            continue;
        patchMesh_t *def = b->patch->def;     // patch_t::def (== b->def->patch)
        iassert( b->patch->def == b->def->patch );
        Patch_TransposeGrid( def );
        patchInvert2( def );
        if ( def->curveDef )
            free( def->curveDef );
        def->curveDef = Patch_GenericMesh2( def, g_qeglobals.current_edit_layer, 0, 0 );
        ++def->version;
        Patch_Rebuild( def, 1 );
    }
}

// ─── Patch_Duplicate (0x4486A0) — deep-copy a patch + its symbiont brush ──────
//   Make a fresh patch, copy the control grid + header, rebuild the tessellated
//   curveDef, and create a NEW symbiont brush (AddBrushForPatch) owned by the source's
//   owner.  Reached by the undo/clone path (Brush_FullClone 0x475E80, Brush_Clone,
//   Brush_CloneComplex), so undo of ANY patch edit routes through here.
patchMesh_t *Patch_Duplicate( patchMesh_t *pFrom )
{
    patchMesh_t *p = MakeNewPatch();
    memcpy( p, pFrom, sizeof( patchMesh_t ) );
    p->curveDef = 0;
    p->curveDef = Patch_GenericMesh2( p, g_qeglobals.current_edit_layer, 0, 0 );
    AddBrushForPatch( p, (entity_s *)pFrom->pSymbiot->owner );
    return p;
}

// used by Patch_Lightmap_Texturing_Sub.
extern void Select_SetTexture( float *out );           // 0x456D70 select.cpp (layer sample size)

// ════════════════════════════════════════════════════════════════════════════
//  CAP / NATURAL TEXTURING CHAIN
//  Patch→Cap (and the cap arm of Patch→Naturalize): project a reference face's texture
//  plane across the whole control grid.  All geometry — the only "material" touch is
//  reading the reference material's texel width/height (MaterialDef_GetLayeredMaterial,
//  real, with a 512 fallback) and the already-real Face_MoveTexture projection.
// ════════════════════════════════════════════════════════════════════════════

int g_nFaceCycle = 0;   // 0x25D5B10 — Patch_GetAxisFace's round-robin face cursor

// 8-connected neighbour directions (di=col, dj=row) — the order sets the cross-product
// winding so the accumulated vertex normal matches the binary.
static const int s_patchNeighborDirs[8][2] =
{ {0,1},{1,1},{1,0},{1,-1},{0,-1},{-1,-1},{-1,0},{-1,1} };

// ── Patch_MeshNormals (0x437c80) — recompute every control point's smooth normal ──
//  For each control point, gather edge vectors to its 8 neighbours (walking out up to 3
//  steps past coincident points), cross adjacent edges to get triangle normals, sum and
//  normalize.  Honors width/height "closed" (cylinder) wrap when the seam columns/rows
//  coincide (dist <= 1).  Output (ctrl[].normal) drives rendering only — not serialized.
static patchMesh_t *Patch_MeshNormals( patchMesh_t *p )
{
    const int width  = p->width;
    const int height = p->height;

    // closed-in-width: first and last COLUMN coincide across all rows (<= 1 unit).
    int closedW = ( height > 0 );
    for ( int j = 0; j < height; ++j )
    {
        const float *a = p->ctrl[0][j].xyz;
        const float *b = p->ctrl[width - 1][j].xyz;
        float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        if ( (float)sqrt( dx * dx + dy * dy + dz * dz ) > 1.0f ) { closedW = 0; break; }
    }
    // closed-in-height: first and last ROW coincide across all columns.
    int closedH = ( width > 0 );
    for ( int i = 0; i < width; ++i )
    {
        const float *a = p->ctrl[i][0].xyz;
        const float *b = p->ctrl[i][height - 1].xyz;
        float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        if ( (float)sqrt( dx * dx + dy * dy + dz * dz ) > 1.0f ) { closedH = 0; break; }
    }

    for ( int col = 0; col < width; ++col )
    {
        for ( int row = 0; row < height; ++row )
        {
            const float *center = p->ctrl[col][row].xyz;
            float edges[8][3];
            int   valid[8];

            for ( int d = 0; d < 8; ++d )
            {
                edges[d][0] = edges[d][1] = edges[d][2] = 0.0f;
                valid[d] = 0;
                const int di = s_patchNeighborDirs[d][0];
                const int dj = s_patchNeighborDirs[d][1];

                for ( int step = 1; step <= 3; ++step )
                {
                    int c = col + di * step;
                    int r = row + dj * step;
                    if ( closedW )
                    {
                        if ( c >= 0 ) { if ( c >= width ) c = c - width + 1; }
                        else            c = c + width - 1;
                    }
                    if ( closedH )
                    {
                        if ( r >= 0 ) { if ( r >= height ) r = r - height + 1; }
                        else            r = r + height - 1;
                    }
                    if ( c < 0 || c >= width || r < 0 || r >= height )
                        break;
                    const float *nb = p->ctrl[c][r].xyz;
                    float e[3] = { nb[0] - center[0], nb[1] - center[1], nb[2] - center[2] };
                    if ( Vec3Normalize_R( e ) != 0.0f )
                    {
                        edges[d][0] = e[0]; edges[d][1] = e[1]; edges[d][2] = e[2];
                        valid[d] = 1;
                        break;
                    }
                }
            }

            float acc[3] = { 0.0f, 0.0f, 0.0f };
            for ( int d = 0; d < 8; ++d )
            {
                int dn = ( d + 1 ) & 7;
                if ( valid[d] && valid[dn] )
                {
                    float fn[3];
                    Vec3Cross( edges[dn], edges[d], fn );
                    if ( Vec3Normalize_R( fn ) != 0.0f )
                    {
                        acc[0] += fn[0]; acc[1] += fn[1]; acc[2] += fn[2];
                    }
                }
            }
            Curve_NormalizeTo( p->ctrl[col][row].normal, acc );
        }
    }
    return p;
}

// ── Patch_CapReferenceFace (0x439990) — pick a cap reference face (first long-edged face) ────
//  Returns the symbiont brush's first face whose first winding edge spans > 8 units in
//  any axis (else face[0]).  The auto-cap projection reference.
static face_t *Patch_CapReferenceFace( patchMesh_t *p )
{
    iassert( p );                                   // 1079
    brush_t *b = (brush_t *)p->pSymbiot;
    iassert( b );                                   // 1081

    if ( b->faceCount == 0 )
        return b->faces;

    for ( int i = 0; i < b->faceCount; ++i )
    {
        winding_t *w = b->faces[i].w;
        iassert( w );                               // 1085
        const float *p0 = w->p[0];
        const float *p1 = w->p[1];
        int spans = ( 8.0f < p1[0] - p0[0] )
                  + ( 8.0f < p1[1] - p0[1] )
                  + ( 8.0f < p1[2] - p0[2] );
        if ( spans > 0 )
            return &b->faces[i];
    }
    return b->faces;
}

// ── Patch_GetAxisFace (0x439ab0) — round-robin a cap face (manual cap cycle) ───
static face_t *Patch_GetAxisFace( patchMesh_t *p )
{
    brush_t *b = (brush_t *)p->pSymbiot;
    unsigned int faceCount = (unsigned int)b->faceCount;

    unsigned int idx = (unsigned int)++g_nFaceCycle;
    unsigned int cap = ( faceCount <= 6 ) ? faceCount : 6;
    if ( idx >= cap ) { idx = 0; g_nFaceCycle = 0; }
    iassert( idx < faceCount );
    if ( idx >= faceCount )
        idx = g_nFaceCycle;
    return &b->faces[idx];
}

// ── Patch_ST (0x4390d0) — project every control point's xyz onto the texture plane ─
//  Build the 2×4 texture projection (Face_MoveTexture) from `params`
//  {sizeX,sizeY,shiftX,shiftY,rotate,crossterm} + the reference face normal, then write
//  each control point's layer S/T.  The low-12-bits "clean" snap removes fp noise.
static void Patch_ST( const float *params, const float *faceNormal, patchMesh_t *p, int layer )
{
    float mat[8];
    Face_MoveTexture( (float *)params, faceNormal, (float *)mat,
                      (float *)( params + 2 ), params[4], params[5] );

    for ( int col = 0; col < p->width; ++col )
    {
        for ( int row = 0; row < p->height; ++row )
        {
            drawVert_t *cp  = &p->ctrl[col][row];
            const float *xyz = cp->xyz;
            float *st = &cp->texCoord.st[2 * layer];

            float s = mat[1] * xyz[1] + mat[0] * xyz[0] + mat[2] * xyz[2] + mat[3];
            unsigned int sb = *(unsigned int *)&s;
            unsigned int slo = sb & 0xFFF;
            if ( slo > 4 ) { if ( (int)( 4096 - slo ) <= 4 ) sb += ( 4096 - slo ); }
            else            sb -= slo;
            s = *(float *)&sb;
            iassert( isfinite( s ) );
            st[0] = s;

            float t = mat[4] * xyz[0] + mat[5] * xyz[1] + mat[6] * xyz[2] + mat[7];
            unsigned int tb = *(unsigned int *)&t;
            unsigned int tlo = tb & 0xFFF;
            if ( tlo > 4 ) { if ( (int)( 4096 - tlo ) <= 4 ) tb += ( 4096 - tlo ); }
            else            tb -= tlo;
            t = *(float *)&tb;
            iassert( isfinite( t ) );
            st[1] = t;
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Patch_ApplyMaterial (0x4428f0) — apply the current material to a PATCH on drag-texture /
//  whole-brush texture apply.  Reached from Brush_SetAllFacesMaterial (brush.cpp) when
//  the brush carries a patch and materialOnly is 0 (the drag-texture-onto-a-patch and
//  patch-instance whole-brush apply paths).  Two things happen:
//   1) Always: stamp the clicked material's two ptrs (lyrMtl/radMtl) into the
//      patch's per-layer texture slots and bump version.  `material` is the SAME
//      MaterialDef block as Brush_SetAllFacesMaterial's.
//   2) Conditionally project: if useStoredST AND the current texture template
//      (random_texture_stuff[layer]) has a stored ST-projection whose grid w/h match
//      the patch's, copy that stored per-control-point ST block (cur->st[col][row], the
//      block Texture_SetTexture writes on a patch pick) onto the patch's control grid,
//      then rebuild the tessellated curveDef.  Otherwise, if planarProject is set,
//      planar-project via Ed_Patch_PlanarTest + Patch_ApplyCapST.  Tail:
//      refresh the patch inspector if open (GUI-only; null headless).
// ════════════════════════════════════════════════════════════════════════════
extern char Ed_Patch_PlanarTest( patchMesh_t *patch, void *outBlk );   // brush.cpp (0x4382D0)
extern bool  PatchDialog_IsOpen();                                     // patchdialog.cpp
extern void  g_PatchDialog_GetPatchInfo();
static curvePatchDef_t *Patch_ApplyCapST( patchMesh_t *p, const float *faceNormal, const float *params ); // (below)

char Patch_ApplyMaterial( MaterialDef *material, patchMesh_t *patch, char planarProject, float useStoredST )
{
    const int layer = g_qeglobals.current_edit_layer;

    // 1) Stamp the material's two ptrs into the patch's per-layer texture slots.
    patchMesh_material *texSlot = &patch->texture + layer;
    texSlot->lyrMtl = material->lyrMtl;
    texSlot->radMtl = material->radMtl;
    ++patch->version;

    // the stored ST-projection block in the current-texture template.
    curTexWndLayer_t *cur = &g_qeglobals.random_texture_stuff[layer];

    if ( useStoredST != 0.0f && cur->hasProjection )
    {
        if ( patch->width  == cur->gridWidth
          && patch->height == cur->gridHeight )
        {
            for ( int col = 0; col < patch->width; ++col )
                for ( int row = 0; row < patch->height; ++row )
                {
                    const float s = cur->st[col][row][0];
                    const float t = cur->st[col][row][1];
                    iassert( isfinite( s ) );
                    iassert( isfinite( t ) );
                    patch->ctrl[col][row].texCoord.st[2 * layer]     = s;
                    patch->ctrl[col][row].texCoord.st[2 * layer + 1] = t;
                }

            if ( layer == 1 )
                patch->bDirty = 0;
            if ( patch->curveDef )
                free( patch->curveDef );
            patch->curveDef = Patch_GenericMesh2( patch, layer, 0, 0 );
            ++patch->version;
        }
    }
    else if ( planarProject )                                                      // planar project
    {
        char planeBlk[32];                                                        // {float N[3]@0, double dist@16}
        if ( Ed_Patch_PlanarTest( patch, planeBlk ) )
        {
            int curLayer = LayerMat::GetCurrentLayer( material );
            Patch_ApplyCapST( patch, (const float *)planeBlk,
                              (const float *)( &material->mat_texDef + curLayer ) );
        }
    }

    if ( PatchDialog_IsOpen() )
        g_PatchDialog_GetPatchInfo();                                             // refresh patch inspector
    return 0;
}

// ── Patch_ApplyCapST (0x439280) — Patch_ST + rebuild the tessellated curveDef ───────
static curvePatchDef_t *Patch_ApplyCapST( patchMesh_t *p, const float *faceNormal, const float *params )
{
    Patch_ST( params, faceNormal, p, g_qeglobals.current_edit_layer );
    if ( g_qeglobals.current_edit_layer == 1 )
        p->bDirty = 0;
    if ( p->curveDef )
        free( p->curveDef );
    curvePatchDef_t *result = Patch_GenericMesh2( p, g_qeglobals.current_edit_layer, 0, 0 );
    ++p->version;
    p->curveDef = result;
    return result;
}

// ─── Patch_CapTexture (0x439B10) — patch CAP texturing ────────────────────────
//  Reached by Patch_NaturalizeSelected's unk=1 branch (the "Apply Patch Cap" command).
//  Recompute mesh normals, pick a reference face (Patch_GetAxisFace round-robin when
//  cap, else Patch_CapReferenceFace's first long face), read its layer material's texel size (512
//  fallback), and project that face's texture plane (scaled by x/y) across the grid.
curvePatchDef_t *Patch_CapTexture( patchMesh_t *p, char cap, float x, float y )
{
    Patch_MeshNormals( p );

    face_t *axisFace = cap ? Patch_GetAxisFace( p ) : Patch_CapReferenceFace( p );

    qtexture_s *lm = MaterialDef_GetLayeredMaterial(
                         &axisFace->mtldef[g_qeglobals.current_edit_layer] );
    int width  = lm ? lm->width  : 512;
    int height = lm ? lm->height : 512;

    float params[6];
    params[0] = (float)width  * x;
    params[1] = (float)height * y;
    params[2] = params[3] = params[4] = params[5] = 0.0f;

    return Patch_ApplyCapST( p, axisFace->plane.normal, params );
}

// ─── Patch_NaturalizeSelected (0x447FD0) — Patch→Naturalize core ──────────────
//   For each selected patch (Undo-bracketed): unk=0 → Patch_Naturalize2 lays linear
//   per-control-point S/T at scale (x,y) on the current edit layer, then rebuilds the
//   tessellated curveDef (free + Patch_GenericMesh2) and bumps version.  unk=1 routes
//   to Patch_CapTexture (cap texturing).  No-op when no patch is selected (scans for
//   the first selected `patch` and returns early if none).
void Patch_NaturalizeSelected( bool unk, bool cap, float x, float y )
{
    selbrush_t *first = selected_brushes.next;
    if ( first == &selected_brushes )
        return;
    while ( !first->patch )
    {
        first = first->next;
        if ( first == &selected_brushes )
            return;
    }

    Undo_ClearRedo();
    Undo_GeneralStart( cap ? "Patch cap texturing" : "Patch natural texturing" );
    Undo_AddBrushList( &selected_brushes );

    for ( selbrush_t *b = selected_brushes.next;
          b != &selected_brushes;
          b = b->next )
    {
        if ( !b->patch )
            continue;
        patchMesh_t *def = b->patch->def;     // patch_t::def (== b->def->patch)
        iassert( b->patch->def == b->def->patch );
        if ( unk )
        {
            Patch_CapTexture( def, (char)cap, x, y );
        }
        else
        {
            Patch_Naturalize2( def, g_qeglobals.current_edit_layer, x, y );
            if ( def->curveDef )
                free( def->curveDef );
            def->curveDef = Patch_GenericMesh2( def, g_qeglobals.current_edit_layer, 0, 0 );
            ++def->version;
        }
    }

    Undo_EndBrushList( &selected_brushes );
    Undo_End();
}

// ─── Patch_Lightmap_Texturing_Sub (0x4397B0) — re-lay a single patch's lightmap-layer S/T ──
//   Reads the current layer's sample size (Select_SetTexture), then projects the lightmap-
//   layer texCoords either by the TERRAIN dominant-axis path (Patch_TerrainTexProject) or
//   the curve sample-size resampler (Patch_ResampleCurveST).  When the edit layer is the lightmap layer
//   (1) it also stamps the sample size into the patch (bDirty + size_of_struct_0x504C).
//   Finally rebuilds the tessellated curveDef and bumps the version.  (The layer passed to
//   Patch_TerrainTexProject/Patch_ResampleCurveST is the current edit layer, not a fixed
//   lightmap layer.)
static curvePatchDef_t *Patch_Lightmap_Texturing_Sub( patchMesh_t *def )
{
    float sampleSize[2];
    Select_SetTexture( sampleSize );

    const int layer = g_qeglobals.current_edit_layer;
    if ( ( def->type & PATCH_TERRAIN ) != 0 )
        Patch_TerrainTexProject( def, layer, sampleSize[0] );
    else
        Patch_ResampleCurveST( def, layer, sampleSize[0] );

    if ( layer == 1 )
    {
        def->bDirty = 1;
        *(float *)&def->size_of_struct_0x504C = sampleSize[0];   // stores the sample size
    }

    if ( def->curveDef )
        free( def->curveDef );
    curvePatchDef_t *result = Patch_GenericMesh2( def, g_qeglobals.current_edit_layer, 0, 0 );
    ++def->version;
    def->curveDef = result;
    return result;
}

// ─── Patch_Lightmap_Texturing (0x448110) — Surface Inspector "Lightmap" for patches ───
//   For each selected patch (Undo-bracketed), re-lays its lightmap-layer texCoords via
//   Patch_Lightmap_Texturing_Sub.  No-op when no patch is selected (scans for the first
//   selected `patch` and returns early if none).
void Patch_Lightmap_Texturing()
{
    selbrush_t *first = selected_brushes.next;
    if ( first == &selected_brushes )
        return;
    while ( !first->patch )
    {
        first = first->next;
        if ( first == &selected_brushes )
            return;
    }

    Undo_ClearRedo();
    Undo_GeneralStart( "Patch lightmap texturing" );
    Undo_AddBrushList( &selected_brushes );

    for ( selbrush_t *b = selected_brushes.next;
          b != &selected_brushes;
          b = b->next )
    {
        if ( !b->patch )
            continue;
        iassert( b->patch->def == b->def->patch );
        Patch_Lightmap_Texturing_Sub( b->patch->def );
    }

    Undo_EndBrushList( &selected_brushes );
    Undo_End();
}

// ════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND AM) — Patch_KiwiLmapAlign
//
//  USER DIRECTIVE, verbatim: "Please set the texture alignment to 'lmap' when
//  doing a fillet curve (for all parts).  It's the setting that makes it lineup
//  perfect."
//
//  THE SAME FORWARDER SHAPE ROUND Q ESTABLISHED and for the same reason: the
//  primitive the directive names, Patch_Lightmap_Texturing_Sub (0x4397B0), is
//  file-static ported internal with IDA-pinned float behaviour, and the PUBLIC
//  spelling of it — Patch_Lightmap_Texturing (0x448110), what the Surface
//  Inspector's "Lmap" button runs — walks `selected_brushes` and opens its OWN
//  Undo_GeneralStart/Undo_End bracket.  Calling that from inside a command's
//  commit would put a second undo record inside one gesture, which is the one
//  thing kiwi_command.h does not allow.  So the per-patch primitive gets a
//  forwarder and the CALLER keeps owning the bracket.
//
//  NOTHING IS REIMPLEMENTED: this is one call to the ported sub, on one patch.
//  It reads the layer sample size through Select_SetTexture and the current edit
//  layer exactly as the button path does, so a patch aligned here and a patch
//  aligned by pressing "Lmap" go through identical code.
//
//  NO PORTED CALLER IS CHANGED.
// ════════════════════════════════════════════════════════════════════════════
void Patch_KiwiLmapAlign( patchMesh_t *p )
{
    if ( !p )
        return;
    Patch_Lightmap_Texturing_Sub( p );
}

// ════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND AN) — Patch_KiwiCapAlign
//
//  USER DIRECTIVE, verbatim: "the main surface needs the 'CAP' button pressed
//  in texture inspector.  Do this automatically please."
//
//  Same forwarder shape and same reason as Patch_KiwiLmapAlign above: the
//  primitive behind the Surface Inspector's CAP button is Patch_CapTexture
//  (0x439B10, this file :2717), but its public route
//  (Patch_NaturalizeSelected's unk=1 arm) walks `selected_brushes` and opens
//  its own undo bracket — unusable inside a command's commit.  One call to the
//  ported primitive at the button path's default 1.0/1.0 scale, plus the same
//  curveDef rebuild Patch_Naturalize does (:2890-2893) because the cap arm's
//  own rebuild is deferred to its caller.
// ════════════════════════════════════════════════════════════════════════════
void Patch_KiwiCapAlign( patchMesh_t *p )
{
    if ( !p )
        return;
    // ROUND AS (crash fix, defense in depth): CAP's reference face comes from the
    // SYMBIONT brush (Patch_GetAxisFace / Patch_CapReferenceFace read p->pSymbiot) — a patch not
    // yet handed to AddBrushForPatch has none, and the ported pickers deref it
    // unguarded (the user's AV: `b was nullptr`).  Callers must align AFTER
    // AddBrushForPatch; this guard turns a future ordering mistake into a console
    // line instead of a crash.
    if ( !p->pSymbiot )
    {
        Sys_Printf( "Patch_KiwiCapAlign: patch has no symbiont brush yet - "
                    "call after AddBrushForPatch; CAP alignment skipped.\n" );
        return;
    }
    Patch_CapTexture( p, 1, 1.0f, 1.0f );
    if ( p->curveDef )
        free( p->curveDef );
    p->curveDef = Patch_GenericMesh2( p, g_qeglobals.current_edit_layer, 0, 0 );
    ++p->version;
}

// ─── Patch_Naturalize (0x439920) — natural texturing at the editor default scale ──
//   Thin wrapper: Patch_Naturalize2 at the current layer's default tex-repeat scale
//   (random_texture_stuff[layer].sampleSize) + curveDef rebuild + ++version.  Used by the
//   Redisperse ops below (re-lays texCoords after the control grid is redistributed).
static void Patch_Naturalize( patchMesh_t *p )
{
    const float scale = g_qeglobals.random_texture_stuff[g_qeglobals.current_edit_layer].sampleSize;
    Patch_Naturalize2( p, g_qeglobals.current_edit_layer, scale, scale );
    if ( p->curveDef )
        free( p->curveDef );
    p->curveDef = Patch_GenericMesh2( p, g_qeglobals.current_edit_layer, 0, 0 );
    ++p->version;
}

// ─── Patch_DisperseColumns (0x4443A0) — Curve→Redisperse Cols (cmd 32889) ─────
//   For each selected patch (Undo-bracketed): rebuild, then redistribute each ROW's
//   control points EVENLY along the chord from column 0 to width-1 (cumulative
//   ctrl[j][r] = ctrl[j-1][r] + (ctrl[w-1][r]-ctrl[0][r])/(w-1)), then re-naturalize once.
void Patch_DisperseColumns()
{
    Undo_ClearRedo();
    Undo_GeneralStart( "disperse columns" );
    Undo_AddBrushList( &selected_brushes );

    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        if ( !b->patch )
            continue;
        patchMesh_t *def = b->patch->def;
        iassert( b->patch->def == b->def->patch );
        Patch_Rebuild( def, 1 );
        for ( int r = 0; r < def->height; ++r )
        {
            if ( def->width <= 1 )
                continue;
            // recip = (float)(1.0/(width-1)); step = recip * delta — a float
            // reciprocal MULTIPLY, not a divide (they differ by 1 ULP).
            float recip = (float)( 1.0 / (double)( def->width - 1 ) );
            float step[3];
            for ( int k = 0; k < 3; ++k )
                step[k] = recip * ( def->ctrl[def->width - 1][r].xyz[k] - def->ctrl[0][r].xyz[k] );
            for ( int j = 1; j < def->width; ++j )
                for ( int k = 0; k < 3; ++k )
                    def->ctrl[j][r].xyz[k] = def->ctrl[j - 1][r].xyz[k] + step[k];
        }
        Patch_Naturalize( def );                 // once per patch
    }

    Undo_EndBrushList( &selected_brushes );
    Undo_End();
    if ( PatchDialog_IsOpen() )               // refresh patch inspector
        g_PatchDialog_GetPatchInfo();
}

// ─── Patch_DisperseRows (0x444200) — Curve→Redisperse Rows (cmd 32888) ────────
//   The row analogue: redistribute each COLUMN's control points along the chord from
//   row 0 to height-1.  NOTE Patch_Naturalize runs PER COLUMN (inside the column loop),
//   not once, as in the binary (the final state is identical, each call fully re-lays
//   texCoords from the current geometry).
void Patch_DisperseRows()
{
    Undo_ClearRedo();
    Undo_GeneralStart( "disperse rows" );
    Undo_AddBrushList( &selected_brushes );

    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        if ( !b->patch )
            continue;
        patchMesh_t *def = b->patch->def;
        iassert( b->patch->def == b->def->patch );
        Patch_Rebuild( def, 1 );
        for ( int c = 0; c < def->width; ++c )
        {
            if ( def->height > 1 )
            {
                // recip = (float)(1.0/(height-1)); step = recip * delta (float
                // reciprocal multiply, not a divide).
                float recip = (float)( 1.0 / (double)( def->height - 1 ) );
                float step[3];
                for ( int k = 0; k < 3; ++k )
                    step[k] = recip * ( def->ctrl[c][def->height - 1].xyz[k] - def->ctrl[c][0].xyz[k] );
                for ( int j = 1; j < def->height; ++j )
                    for ( int k = 0; k < 3; ++k )
                        def->ctrl[c][j].xyz[k] = def->ctrl[c][j - 1].xyz[k] + step[k];
            }
            Patch_Naturalize( def );             // per-column
        }
    }

    Undo_EndBrushList( &selected_brushes );
    Undo_End();
    if ( PatchDialog_IsOpen() )               // refresh patch inspector
        g_PatchDialog_GetPatchInfo();
}

// ─── Patch_InvertTexture (0x446680) — Curve→Negative Texture X/Y (32899/32903) ─
//   Mirror the CURRENT edit layer's texture-coord (st) pair of every selected patch:
//     axis 0 (TextureX, 32899) mirrors across ROWS    (ctrl[c][j] <-> ctrl[c][h-1-j]);
//     axis 1 (TextureY, 32903) mirrors across COLUMNS (ctrl[i][r] <-> ctrl[w-1-i][r]).
//   xyz is untouched (texture-only flip); curveDef rebuilt + ++version per patch.  An
//   involution — inverting the same axis twice restores the original st.  The per-element
//   IS_NAN guard asserts (5913/5914/5916/5917 columns, 5929/5930/5932/5933 rows; LEVEL 0,
//   defensive-only — never fire for valid st) are interleaved with the swap as in the
//   binary: assert the source (a) before c=a, the saved temp before a=fTemp.
void Patch_InvertTexture( char axis )
{
    const int layer = g_qeglobals.current_edit_layer;
    char did = 0;
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        if ( !b->patch )
            continue;
        patchMesh_t *def = b->patch->def;
        iassert( b->patch->def == b->def->patch );
        did = 1;
        if ( axis )                                  // mirror COLUMNS (TextureY)
        {
            for ( int r = 0; r < def->height; ++r )
                for ( int i = 0; i < def->width / 2; ++i )
                {
                    float *a = &def->ctrl[i][r].texCoord.st[2 * layer];
                    float *c = &def->ctrl[def->width - 1 - i][r].texCoord.st[2 * layer];
                    float t0 = c[0], t1 = c[1];
                    iassert( isfinite( a[0] ) && isfinite( a[1] ) );
                    c[0] = a[0]; c[1] = a[1];
                    iassert( isfinite( t0 ) && isfinite( t1 ) );
                    a[0] = t0;   a[1] = t1;
                }
        }
        else                                         // mirror ROWS (TextureX)
        {
            for ( int col = 0; col < def->width; ++col )
                for ( int j = 0; j < def->height / 2; ++j )
                {
                    float *a = &def->ctrl[col][j].texCoord.st[2 * layer];
                    float *c = &def->ctrl[col][def->height - 1 - j].texCoord.st[2 * layer];
                    float t0 = c[0], t1 = c[1];
                    iassert( isfinite( a[0] ) && isfinite( a[1] ) );
                    c[0] = a[0]; c[1] = a[1];
                    iassert( isfinite( t0 ) && isfinite( t1 ) );
                    a[0] = t0;   a[1] = t1;
                }
        }
        if ( layer == 1 )
            def->bDirty = 0;
        if ( def->curveDef )
            free( def->curveDef );
        def->curveDef = Patch_GenericMesh2( def, layer, 0, 0 );
        ++def->version;
    }
    if ( did )
        g_nUpdateBits = -1;
    if ( PatchDialog_IsOpen() )               // refresh patch inspector
        g_PatchDialog_GetPatchInfo();
}

// ─── Patch_Scale (0x4427D0) — scale selected patch control points about `mid` ──
//   For each control point (in curve-point edit mode: only those in d_move_points[];
//   otherwise all): xyz[k] = (xyz[k] - mid[k]) * scale[k] + mid[k].  Optionally rebuilds.
//   Reached from Select_Scale's patch branch.  The trailing patch-inspector refresh
//   (CWnd_PatchDialog) goes through the GUI shim (no-op headless).
void Patch_Scale( patchMesh_t *p, const float *mid, const float *scale, char doRebuild )
{
    for ( int w = 0; w < p->width; ++w )
        for ( int row = 0; row < p->height; ++row )
        {
            if ( g_qeglobals.d_select_mode == sel_curvepoint )
            {
                bool selected = false;
                for ( int m = 0; m < g_qeglobals.d_num_move_points; ++m )
                    if ( &p->ctrl[w][row] == g_qeglobals.d_move_points[m] ) { selected = true; break; }
                if ( !selected )
                    continue;                    // not a picked control point → leave it
            }
            for ( int k = 0; k < 3; ++k )
                p->ctrl[w][row].xyz[k] = ( p->ctrl[w][row].xyz[k] - mid[k] ) * scale[k] + mid[k];
        }
    if ( doRebuild )
        Patch_Rebuild( p, 1 );
    if ( PatchDialog_IsOpen() )               // refresh patch inspector
        g_PatchDialog_GetPatchInfo();
}

// ─── Patch_Move (0x441dd0) — translate an ENTIRE patch by a world delta ─────────
//   For every control point in the width×height grid: xyz[k] += move[k].  Unlike
//   Patch_Scale this is unconditional (no curve-point filter) — it is the whole-patch
//   translation reached from Brush_Move (the patch branch of MoveSelection).  After
//   translating, free + rebuild the tessellated curveDef (the brush.cpp:5341 /
//   Patch_ShiftTexture regen) and bump version.  Tail: the patch-inspector refresh
//   (CWnd_PatchDialog) is GUI-only (no-op headless); the regenerated curveDef is returned.
curvePatchDef_t *Patch_Move( const float *move, patchMesh_t *p )
{
    for ( int col = 0; col < p->width; ++col )
        for ( int row = 0; row < p->height; ++row )
        {
            p->ctrl[col][row].xyz[0] += move[0];
            p->ctrl[col][row].xyz[1] += move[1];
            p->ctrl[col][row].xyz[2] += move[2];
        }

    if ( p->curveDef )
        free( p->curveDef );
    p->curveDef = Patch_GenericMesh2( p, g_qeglobals.current_edit_layer, 0, 0 );
    ++p->version;

    // The binary returned the dialog refresh's result when the dialog was open; this always
    // returns the new curveDef.
    if ( PatchDialog_IsOpen() )
        g_PatchDialog_GetPatchInfo();
    return p->curveDef;
}

// ─── Patch_DragScale (0x442b90) — drag-resize a patch (Drag_MoveSelection box edge) ──
//   Scale the patch so its bounding box grows by `vAmt` (the per-axis size delta the drag
//   added), then re-centre it back over `vMove`'s sign.  Algorithm (GtkRadiant Patch_DragScale):
//     1. vTemp = bbox(max-min).  If scaling an axis that has no depth (vTemp[i]==0) while the
//        drag moved that axis (vMove[i]!=0) → bail (return 0): can't scale a degenerate axis.
//     2. vMid = min + vTemp/2;  vScale[i] = vAmt[i] ? 1 + vAmt[i]/vTemp[i] : 1.
//     3. Patch_Scale(patch, vMid, vScale, 0) — scale about the box centre (no rebuild).
//     4. Re-bbox; vShift = (newExtent - vTemp)/2 (half the size change per axis).  For each
//        axis where the box actually grew (sq diff > 1e-6) negate vShift[i] when the drag
//        pulled the LOW edge (vAmt[i] != vMove[i]); then Patch_Move(vShift, patch) to keep
//        the dragged edge anchored.  If no axis grew, Patch_Move(vShift, patch) with the raw
//        (positive) half-shift.  Returns 1 on success.
//   Reached only from drag.cpp:798 (MoveSelection patch resize).
int Patch_DragScale( float *vAmt, void *patchDefV, float *vMove )
{
    patchMesh_t *patch = (patchMesh_t *)patchDefV;
    float vMin[3], vMax[3], vMid[3], vScale[3], vTemp[3];

    Patch_CalcBounds( vMin, vMax, patch, 0 );
    for ( int i = 0; i < 3; ++i )
        vTemp[i] = vMax[i] - vMin[i];

    // degenerate-axis bail: can't scale a zero-depth axis the drag is trying to move.
    for ( int i = 0; i < 3; ++i )
        if ( vTemp[i] == 0.0f && vMove[i] != 0.0f )
            return 0;

    for ( int i = 0; i < 3; ++i )
        vMid[i] = vMin[i] + vTemp[i] * 0.5f;
    for ( int i = 0; i < 3; ++i )
        vScale[i] = ( vAmt[i] != 0.0f ) ? ( vAmt[i] / vTemp[i] + 1.0f ) : 1.0f;

    Patch_Scale( patch, vMid, vScale, 0 );

    // re-measure and compute the half-extent change per axis.
    Patch_CalcBounds( vMin, vMax, patch, 0 );
    float vShift[3];
    for ( int i = 0; i < 3; ++i )
        vShift[i] = ( ( vMax[i] - vMin[i] ) - vTemp[i] ) * 0.5f;

    // find the first axis that actually grew (squared diff above the 1e-6 epsilon).
    int grew = 3;
    for ( int i = 0; i < 3; ++i )
    {
        float d = vMove[i] - vAmt[i];
        if ( d * d > 9.9999998e-7f ) { grew = i; break; }
    }
    if ( grew < 3 )
    {
        // anchor the dragged edge: negate the shift on any axis pulled from its low edge.
        if ( vAmt[0] != vMove[0] ) vShift[0] = -vShift[0];
        if ( vAmt[1] != vMove[1] ) vShift[1] = -vShift[1];
        if ( vAmt[2] != vMove[2] ) vShift[2] = -vShift[2];
    }
    Patch_Move( vShift, patch );
    return 1;
}

// ─── Patch_ShiftTexture (0x446170) — shift selected patch control-point texcoords ──
//   Adds (s,t) to each control point's current-layer st (curve-point mode → only the ones
//   in d_move_points[]; otherwise all).  A |shift| >= 1 is scaled down by 10 first (coarse
//   step → fine nudge), then curveDef is rebuilt.  Reached from Brush_ShiftTexture's patch
//   branch.
void Patch_ShiftTexture( patchMesh_t *p, float s, float t )
{
    // fabs( shift ) >= 1.0 → shift / 10.0 (double fabs + double divide).
    if ( fabs( (double)s ) >= 1.0 ) s = (float)( (double)s / 10.0 );
    if ( fabs( (double)t ) >= 1.0 ) t = (float)( (double)t / 10.0 );
    const int layer = g_qeglobals.current_edit_layer;
    for ( int w = 0; w < p->width; ++w )
        for ( int h = 0; h < p->height; ++h )
        {
            if ( g_qeglobals.d_select_mode == sel_curvepoint )
            {
                bool selected = false;
                for ( int m = 0; m < g_qeglobals.d_num_move_points; ++m )
                    if ( &p->ctrl[w][h] == g_qeglobals.d_move_points[m] ) { selected = true; break; }
                if ( !selected )
                    continue;
            }
            p->ctrl[w][h].texCoord.st[2 * layer]     += s;
            iassert( isfinite( p->ctrl[w][h].texCoord.st[2 * layer] ) );
            p->ctrl[w][h].texCoord.st[2 * layer + 1] += t;
            iassert( isfinite( p->ctrl[w][h].texCoord.st[2 * layer + 1] ) );
        }
    if ( layer == 1 )
        p->bDirty = 0;
    if ( p->curveDef )
        free( p->curveDef );
    p->curveDef = Patch_GenericMesh2( p, layer, 0, 0 );
    ++p->version;
}

// ─── Patch_RotateTexture (0x445B80) — rotate selected patch control-point texcoords ──
//   Rotate each control point's current-layer st by `deg` degrees about the texel-space
//   origin: sTex=st0*texW, tTex=st1*texH; (ns,nt)=rot(sTex,tTex); st0=ns/texW, st1=nt/texH.
//   texW/texH come from the layer's layered material (512 default).  Curve-point mode → only
//   the d_move_points[]; else all.  Reached from Brush_RotateTexture's patch branch.
void Patch_RotateTexture( patchMesh_t *p, float deg )
{
    const int layer = g_qeglobals.current_edit_layer;
    qtexture_s *lm = MaterialDef_GetLayeredMaterial( (MaterialDef *)( &p->texture + layer ) );
    float texW, texH;
    if ( lm )
    {
        texW = (float)lm->width;
        texH = (float)lm->height;
        vassert( lm->width > 0, "(texSize[0]) = %i", lm->width );
        vassert( lm->height > 0, "(texSize[1]) = %i", lm->height );
    }
    else
    {
        texW = 512.0f;
        texH = 512.0f;
    }

    // KIWI: the binary's Patch_CalcBounds call here was output-unused (the version bump below
    // stands).  Double angle, exact at multiples of 90 (kiwi_angles.h) - cos( (float)(PI / 2) )
    // is -4.4e-8, so four 90 degree turns did not give the texture coords back.
    double sd, cd;
    KiwiAng_SinCos( (double)deg, &sd, &cd );
    const float c = (float)cd, s = (float)sd;

    for ( int w = 0; w < p->width; ++w )
        for ( int h = 0; h < p->height; ++h )
        {
            if ( g_qeglobals.d_select_mode == sel_curvepoint )
            {
                bool selected = false;
                for ( int m = 0; m < g_qeglobals.d_num_move_points; ++m )
                    if ( &p->ctrl[w][h] == g_qeglobals.d_move_points[m] ) { selected = true; break; }
                if ( !selected )
                    continue;
            }
            float sTex = p->ctrl[w][h].texCoord.st[2 * layer]     * texW;
            float tTex = p->ctrl[w][h].texCoord.st[2 * layer + 1] * texH;
            float ns   = c * sTex - s * tTex;
            float nt   = sTex * s + tTex * c;
            iassert( isfinite( ns ) );
            iassert( isfinite( nt ) );
            p->ctrl[w][h].texCoord.st[2 * layer]     = ns / texW;
            p->ctrl[w][h].texCoord.st[2 * layer + 1] = nt / texH;
        }

    if ( layer == 1 )
        p->bDirty = 0;
    if ( p->curveDef )
        free( p->curveDef );
    p->curveDef = Patch_GenericMesh2( p, layer, 0, 0 );
    ++p->version;
}

// ─── Patch_ScaleTexture (0x445F80) — scale selected patch control-point texcoords ──
//   Multiply each control point's current-layer st by (sx,sy) — st0*=sx, st1*=sy (a zero
//   factor is treated as 1).  Curve-point mode → only the d_move_points[]; else all.  Then
//   rebuild curveDef.  Layer 1 (lightmap) additionally: when sx==sy, scale the per-patch
//   lightmap-scale accumulator (the size_of_struct_0x504C field, used as a float) by sx and
//   set bDirty; on a non-uniform scale clear bDirty.  Reached from Brush_ScaleTexture's patch
//   branch.
void Patch_ScaleTexture( patchMesh_t *p, float sx, float sy )
{
    if ( sx == 0.0f ) sx = 1.0f;
    if ( sy == 0.0f ) sy = 1.0f;
    const int layer = g_qeglobals.current_edit_layer;
    for ( int w = 0; w < p->width; ++w )
        for ( int h = 0; h < p->height; ++h )
        {
            if ( g_qeglobals.d_select_mode == sel_curvepoint )
            {
                bool selected = false;
                for ( int m = 0; m < g_qeglobals.d_num_move_points; ++m )
                    if ( &p->ctrl[w][h] == g_qeglobals.d_move_points[m] ) { selected = true; break; }
                if ( !selected )
                    continue;
            }
            p->ctrl[w][h].texCoord.st[2 * layer]     *= sx;
            iassert( isfinite( p->ctrl[w][h].texCoord.st[2 * layer] ) );
            p->ctrl[w][h].texCoord.st[2 * layer + 1] *= sy;
            iassert( isfinite( p->ctrl[w][h].texCoord.st[2 * layer + 1] ) );
        }
    if ( sx == sy )
    {
        if ( layer == 1 )
        {
            p->bDirty = 1;
            // per-patch lightmap-scale accumulator (a float aliased over the
            // size_of_struct_0x504C field).
            *(float *)&p->size_of_struct_0x504C = sx * *(float *)&p->size_of_struct_0x504C;
        }
    }
    else if ( layer == 1 )
    {
        p->bDirty = 0;
    }
    if ( p->curveDef )
        free( p->curveDef );
    p->curveDef = Patch_GenericMesh2( p, layer, 0, 0 );
    ++p->version;
}

// ─── Patch_SetTextureInfo (0x447760) — apply a relative texdef change to the selection ──
//   For every selected patch, dispatch to rotate / shift / scale by whichever texdef field
//   is non-zero (the Patch Inspector's texdef spin arrows feed one field at a time).
void Patch_SetTextureInfo( texdef_sub_t *texDef )
{
    for ( selbrush_t *pb = selected_brushes.next; pb != &selected_brushes; pb = pb->next )
    {
        patch_t *patch = pb->patch;
        if ( !patch )
            continue;
        iassert( patch->def == pb->def->patch );
        patchMesh_t *def = patch->def;
        if ( texDef->rotate != 0.0f )
            Patch_RotateTexture( def, texDef->rotate );
        if ( texDef->shift[0] != 0.0f || texDef->shift[1] != 0.0f )
            Patch_ShiftTexture( def, texDef->shift[0], texDef->shift[1] );
        if ( texDef->size[0] != 0.0f || texDef->size[1] != 0.0f )
            Patch_ScaleTexture( def, texDef->size[0], texDef->size[1] );
    }
}

// ─── Patch_SetTexturing (0x446b60) — "Set..." texture layout across the selection ──
//   Lay texcoords across each selected patch on the current layer.  mode:
//     2 (or non-terrain): parametric GRID — s = col*sx/(w-1), t = -row*sy/(h-1).
//     1: ARC-LENGTH — s/t follow the cumulative 3D edge length along width / height.
//     0: NATURAL — like arc-length but measured in the two dominant world axes (chosen
//        from the patch's bbox extents), giving a world-scaled projection.
static float Patch_TexLen3( const float *a, const float *b )
{
    const float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
    return (float)sqrt( dx * dx + dy * dy + dz * dz );
}
static float Patch_TexLen2( const float *a, const float *b, int i0, int i1 )
{
    const float d0 = b[i0] - a[i0], d1 = b[i1] - a[i1];
    return (float)sqrt( d0 * d0 + d1 * d1 );
}
void Patch_SetTexturing( float sx, float sy, int mode )
{
    // bail if nothing in the selection owns a patch
    selbrush_t *scan = selected_brushes.next;
    while ( scan != &selected_brushes && !scan->patch )
        scan = scan->next;
    if ( scan == &selected_brushes )
        return;

    Undo_ClearRedo();
    Undo_GeneralStart( "Patch set texturing" );
    Undo_AddBrushList( &selected_brushes );

    const int layer = g_qeglobals.current_edit_layer;
    for ( selbrush_t *pb = selected_brushes.next; pb != &selected_brushes; pb = pb->next )
    {
        patch_t *patch = pb->patch;
        if ( !patch )
            continue;
        iassert( patch->def == pb->def->patch );
        patchMesh_t *def = patch->def;
        const int W = def->width, H = def->height;

        if ( mode == 2 || ( def->type & 0x40 ) == 0 )
        {
            for ( int col = 0; col < W; ++col )
                for ( int row = 0; row < H; ++row )
                {
                    // col*sx/(W-1) is computed UNCONDITIONALLY in double (a degenerate
                    // 1-wide/tall patch divides by zero -> the IS_NAN asserts below catch it).
                    float s = (float)( (double)col *   sx   / (double)( W - 1 ) );
                    float t = (float)( (double)row * ( -sy ) / (double)( H - 1 ) );
                    iassert( isfinite( s ) );
                    iassert( isfinite( t ) );
                    def->ctrl[col][row].texCoord.st[2 * layer]     = s;
                    def->ctrl[col][row].texCoord.st[2 * layer + 1] = t;
                }
        }
        else
        {
            // NATURAL: choose the two dominant world axes from the bbox extents.
            int ax0 = 0, ax1 = 1;
            if ( mode == 0 )
            {
                const float *mn = pb->def->mins, *mx = pb->def->maxs;
                const float ex = mx[0] - mn[0], ey = mx[1] - mn[1], ez = mx[2] - mn[2];
                if ( ex > ez * 0.5f && ey > ez * 0.5f ) { ax0 = 0; ax1 = 1; }     // X,Y
                else                                    { ax0 = ( ex <= ey ) ? 1 : 0; ax1 = 2; }  // (Y|X),Z
            }
            // s along width
            for ( int row = 0; row < H; ++row )
            {
                float total = 0.0f;
                for ( int col = 0; col < W - 1; ++col )
                    total += ( mode == 1 ) ? Patch_TexLen3( def->ctrl[col][row].xyz, def->ctrl[col + 1][row].xyz )
                                           : Patch_TexLen2( def->ctrl[col][row].xyz, def->ctrl[col + 1][row].xyz, ax0, ax1 );
                if ( total == 0.0f ) total = 1.0f;
                def->ctrl[0][row].texCoord.st[2 * layer] = 0.0f;
                float cum = 0.0f;
                for ( int col = 1; col < W; ++col )
                {
                    cum += ( mode == 1 ) ? Patch_TexLen3( def->ctrl[col - 1][row].xyz, def->ctrl[col][row].xyz )
                                         : Patch_TexLen2( def->ctrl[col - 1][row].xyz, def->ctrl[col][row].xyz, ax0, ax1 );
                    float newS = sx / total * cum;
                    iassert( isfinite( newS ) );
                    def->ctrl[col][row].texCoord.st[2 * layer] = newS;
                }
            }
            // t along height
            for ( int col = 0; col < W; ++col )
            {
                float total = 0.0f;
                for ( int row = 0; row < H - 1; ++row )
                    total += ( mode == 1 ) ? Patch_TexLen3( def->ctrl[col][row].xyz, def->ctrl[col][row + 1].xyz )
                                           : Patch_TexLen2( def->ctrl[col][row].xyz, def->ctrl[col][row + 1].xyz, ax0, ax1 );
                if ( total == 0.0f ) total = 1.0f;
                def->ctrl[col][0].texCoord.st[2 * layer + 1] = 0.0f;
                float cum = 0.0f;
                for ( int row = 1; row < H; ++row )
                {
                    cum += ( mode == 1 ) ? Patch_TexLen3( def->ctrl[col][row - 1].xyz, def->ctrl[col][row].xyz )
                                         : Patch_TexLen2( def->ctrl[col][row - 1].xyz, def->ctrl[col][row].xyz, ax0, ax1 );
                    float newT = ( -sy ) / total * cum;
                    iassert( isfinite( newT ) );
                    def->ctrl[col][row].texCoord.st[2 * layer + 1] = newT;
                }
            }
        }

        if ( layer == 1 )
            def->bDirty = 0;
        if ( def->curveDef )
            free( def->curveDef );
        def->curveDef = Patch_GenericMesh2( def, layer, 0, 0 );
        ++def->version;
    }
    Undo_EndBrushList( &selected_brushes );
    Undo_End();
}

// ─── Patch_FitTexturing (0x447600) — "Fit" texture onto a patch ──────────────────
//   Lay a single texture tile across the whole patch on the current layer: the s/t of
//   every control point becomes its NORMALIZED grid position — s = col/(width-1) runs
//   0→1 across columns, t = -row/(height-1) runs 0→-1 down rows (negative t).  No
//   curve-point gating (unlike Shift/Scale/Rotate) — every control point is set.  Then
//   clear bDirty on the lightmap layer and free/rebuild curveDef + bump version.  Reached
//   from Brush_FitTextureFaces (0x47C950) when the brush owns a patch (the Surface
//   Inspector "Fit" path).  The binary returns the rebuilt curveDef but every caller
//   ignores it, so this is void (cf. Patch_ScaleTexture).
void Patch_FitTexturing( patchMesh_t *p )
{
    const int layer = g_qeglobals.current_edit_layer;
    for ( int col = 0; col < p->width; ++col )
        for ( int row = 0; row < p->height; ++row )
        {
            float s = (float)(  (double)col / (double)( p->width  - 1 ) );  // double divide
            float t = (float)( -(double)row / (double)( p->height - 1 ) );
            iassert( isfinite( s ) );
            iassert( isfinite( t ) );
            p->ctrl[col][row].texCoord.st[2 * layer]     = s;
            p->ctrl[col][row].texCoord.st[2 * layer + 1] = t;
        }
    if ( layer == 1 )
        p->bDirty = 0;
    if ( p->curveDef )
        free( p->curveDef );
    p->curveDef = Patch_GenericMesh2( p, layer, 0, 0 );
    ++p->version;
}

// ─── patch "undo snapshot" globals (0x231F558.. — a partial patchMesh_t image) ───
//   Written by Patch_ApplyMatrix and Patch_InsDelToggle; the binary read them back
//   in Patch_Restore (0x446B20) / Patch_BendHandleESC (0x447C00) - the cancel paths, neither
//   ported.  ctrl image = 0x5000 bytes = the full 16×16 control grid (256 drawVert_t × 80).
int        patchSave_width;                 // 0x231F558
int        patchSave_height;                // 0x231F55C
int        patchSave_type;                  // 0x231F568
drawVert_t patchSave_ctrl[16][16];          // 0x231F590  (0x5000 bytes)

// ─── Patch_ApplyMatrix (0x441E70) — transform a patch's control points by an orientation ─
//   The patch arm of Select_ApplyMatrix (rotate / flip / scale of a selection containing a
//   patch).  Snapshot
//   the current grid into patchSave_*, then push every control point's xyz through
//   OrientationPosToWorldPos (rel = xyz - origin; xyz = origin + axisᵀ·rel) so it rotates/
//   reflects about orient->origin.  In curve-point mode only the d_move_points[] are
//   moved; otherwise every control point.  `snap` rounds each coord to the nearest half-unit
//   (floor(0.5 + 2·c)·0.5).  Finally Patch_Rebuild(p,1) rebuilds the bbox + curveDef (++version).
void Patch_ApplyMatrix( const orientation_t *orient, patchMesh_t *p, char snap )
{
    patchSave_width  = p->width;
    patchSave_height = p->height;
    patchSave_type   = p->type;
    memcpy( patchSave_ctrl, p->ctrl, 0x5000u );

    for ( int col = 0; col < p->width; ++col )
        for ( int row = 0; row < p->height; ++row )
        {
            if ( g_qeglobals.d_select_mode == sel_curvepoint )   // (the binary: or bend mode)
            {
                bool selected = false;
                for ( int m = 0; m < g_qeglobals.d_num_move_points; ++m )
                    if ( &p->ctrl[col][row] == g_qeglobals.d_move_points[m] ) { selected = true; break; }
                if ( !selected )
                    continue;
            }
            float *xyz = p->ctrl[col][row].xyz;
            float rel[3];
            rel[0] = xyz[0] - orient->origin[0];
            rel[1] = xyz[1] - orient->origin[1];
            rel[2] = xyz[2] - orient->origin[2];
            OrientationPosToWorldPos( xyz, rel, orient );
            // KIWI: noise-only, as Brush_SnapPlanepts (engine_stubs.cpp) - the binary rounds
            // every rotated / flipped control point to 0.5, which bends off-grid patches.
            if ( snap )
            {
                double scale = sqrt( (double)rel[0] * rel[0] + (double)rel[1] * rel[1] + (double)rel[2] * rel[2] );
                for ( int i = 0; i < 3; ++i )   // the turn radius or the point, whichever is larger
                    xyz[i] = KiwiSnap_NoiseHalf( xyz[i], scale );
            }
        }

    Patch_Rebuild( p, 1 );
}

// ════════════════════════════════════════════════════════════════════════════
//  PATCH CONTROL-POINT INTERACTIVE EDIT (bend/drag) - the patch sibling of the
//  VERTEX/EDGE editing (select.cpp SelectVertexByRay/Select_Edge + MoveSelection).  Flow:
//    1. enter curve-point mode (Selection->Drag Vertices on a pure-patch selection ->
//       OnSelectionDragVertices -> Patch_EditPatch) - fills g_qeglobals.d_points[]
//       with every selected patch's control-point world positions + sets sel_curvepoint.
//    2. click a control point (XY_MouseDown -> Drag_Begin -> Drag_Setup ->
//       SelectCurvePointByRay -> Patch_ClickControlPoint) - picks the nearest
//       d_points handle and pushes its drawVert_t* onto g_qeglobals.d_move_points[].
//    3. drag (Drag_MouseMoved -> MoveSelection sel_curvepoint branch ->
//       Patch_UpdateSelected / _0) - adds the snapped world delta to each
//       selected control point, rebuilds d_points, re-tessellates via Patch_Rebuild.
//  With m_bTextureLock OFF each dragged control point's S/T follow the move so the texture
//  stays put in world space (Patch_TexFollow).
// ════════════════════════════════════════════════════════════════════════════

#include "prefs.h"     // g_PrefsDlg (m_bTextureLock / g_bPatchWeld / patch_drill_down)
#include "mainfrm.h"   // CMainFrame / CXYWnd (g_pParentWnd->m_pActiveXY->m_nViewType)
#include "xywnd.h"     // xywndState_t / Ed_ActiveXY (U-GLOBALS)

// patch_t (the patch INSTANCE node) is the shared struct in qe3.h.


// drag/mode globals (defined in drag.cpp / mainfrm.cpp); g_bXYViewIsLastPatchClick is
// defined here (its primary reader is Patch_ClickControlPoint) so the click path can see
// the "second click in the same view" flag XY_MouseDown sets before Drag_Begin.
char         g_bXYViewIsLastPatchClick = 0;     // 0x25D5A6A (def here; extern in xywnd.cpp)
extern int   g_nPatchClickedView;               // 0x73B108 (engine_stubs.cpp)

// ── Patch_FindMovePoint (0x43c2c0) ────────────────────────────────────────────
// Index of `pt` (a vec3 — a control point's xyz, OR a d_points[] slot reinterpreted)
// in g_qeglobals.d_move_points[] by exact xyz match (epsilon 9.5e-07/axis).  brush.cpp
// has a file-static twin (Patch_FindSelectedMovePoint) for the
// marker overlay; this is the select-path copy.
static int Patch_FindMovePoint( const float *pt )
{
    for ( int i = 0; i < g_qeglobals.d_num_move_points; ++i )
    {
        const float *mp = g_qeglobals.d_move_points[i]->xyz;
        int k = 0;
        for ( ; k < 3; ++k )
        {
            float d = pt[k] - mp[k];
            if ( d * d > 1.00000011e-06f )
                break;
        }
        if ( k == 3 )
            return i;
    }
    return -1;
}

// ── Patch_FindMovePointByPtr ──────────────────────────────────────────────────
// Index of the exact control-point POINTER `cp` in d_move_points[], else -1.  The
// binary's move-point dedup/membership checks (Patch_ColumnAllSelected, the
// Patch_SelectCtrlPoint weld+drill-down sweeps, the Patch_ClickControlPoint
// already-selected test) all compare the drawVert_t POINTER, NOT xyz — coincident seam
// points share xyz but are DISTINCT pointers, so the weld sweep must queue each one
// (an xyz match via Patch_FindMovePoint would skip every coincident point after the first
// and defeat the weld).  Patch_FindMovePoint (xyz) remains correct only for
// Patch_RemoveMovePoint (which the binary likewise drives by xyz).
static int Patch_FindMovePointByPtr( const drawVert_t *cp )
{
    for ( int i = 0; i < g_qeglobals.d_num_move_points; ++i )
        if ( g_qeglobals.d_move_points[i] == cp )
            return i;
    return -1;
}

// ── Patch_LocateCtrlPoint (0x43ce60) ───────────────────────────────
// Given a control-point pointer (a drawVert_t* inside some selected patch's ctrl grid),
// find which selected patch owns it and decode its (row,col).  Returns 1 + fills
// out* on success, 0 if the pointer is not inside any selected patch's 16×16 ctrl grid.
// (row = idx%16, col = idx/16, where idx is the drawVert_t index from &ctrl[0][0].)
static int Patch_LocateCtrlPoint( const void *cp, patchMesh_t **outDef, int *outRow, int *outCol )
{
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        if ( !b->patch )
            continue;
        patchMesh_t *def = b->patch->def;
        const char *base = (const char *)&def->ctrl[0][0];
        const char *last = (const char *)&def->ctrl[15][15];
        if ( (const char *)cp >= base && (const char *)cp <= last )
        {
            // KIWI: from &ctrl[0][0]; the binary's `- 56` is x86's ctrl offset.
            int idx = (int)( ( (const char *)cp - base ) / sizeof( drawVert_t ) );
            *outRow = idx % 16;
            *outCol = idx / 16;
            *outDef = def;
            return 1;
        }
    }
    return 0;
}

// ── Patch_AppendMovePoint (0x43c3e0) ───────────────────────────────
// Append a control point (drawVert_t*) to g_qeglobals.d_move_points[].  If a move-point
// belonging to the SAME owning brush is already queued AND more than one brush is
// selected, insert at the FRONT instead (so one patch's points group at the head — the
// drill-down weld ordering).  `def->pSymbiot` holds the symbiont brush_t* (== a
// selbrush_t.def — see AddBrushForPatch).
static void Patch_AppendMovePoint( patchMesh_t *def, drawVert_t *cp )
{
    if ( g_qeglobals.d_num_move_points
         && (brush_t *)def->pSymbiot == selected_brushes.next->def
         && selected_brushes.next != &selected_brushes )
    {
        memmove( &g_qeglobals.d_move_points[1], &g_qeglobals.d_move_points[0],
                 sizeof( g_qeglobals.d_move_points[0] ) * g_qeglobals.d_num_move_points );
        ++g_qeglobals.d_num_move_points;
        g_qeglobals.d_move_points[0] = cp;
    }
    else
    {
        g_qeglobals.d_move_points[g_qeglobals.d_num_move_points++] = cp;
    }
}

// ── Patch_RemoveMovePoint (0x43c320) ───────────────────────────────
// Remove the move-point whose xyz matches `pt` (a d_points slot) from d_move_points[]
// (repeatedly — there can be coincident duplicates from a weld).  The de-select-on-
// reclick path.
static void Patch_RemoveMovePoint( const float *pt )
{
    int idx = Patch_FindMovePoint( pt );
    while ( idx >= 0 )
    {
        for ( ; idx < g_qeglobals.d_num_move_points - 1; ++idx )
            g_qeglobals.d_move_points[idx] = g_qeglobals.d_move_points[idx + 1];
        --g_qeglobals.d_num_move_points;
        idx = Patch_FindMovePoint( pt );
    }
}

// ── Patch_SelectCtrlPoint (0x43c440) ─────────────────────────────────
// Queue control point `cp` (a drawVert_t* in patch `def`'s ctrl grid) as a drag
// move-point (Patch_AppendMovePoint), then — when the weld/drill-down prefs are on
// (g_bPatchWeld / patch_drill_down, both default 1) and `processWeld` is set — sweep
// every selected patch for COINCIDENT control points and queue them too (weld: same
// xyz within 1e-6; drill-down: same projected screen position in the clicked view)
// so a seam moves as one.  For a plain (unseamed) patch nothing extra is queued.
static void Patch_SelectCtrlPoint( patchMesh_t *def, drawVert_t *cp, char processWeld )
{
    // The clicked view's two on-screen axes (the depth axis is excluded for drill-down).
    int nViewType = Ed_ActiveXY()->m_nViewType;   // never NULL — no shell fallback
    int axisA = ( nViewType == 0 );               // first screen axis index
    int axisB = ( nViewType != 2 ) + 1;           // second screen axis index

    Patch_AppendMovePoint( def, cp );

    if ( !processWeld )
        return;
    if ( !g_PrefsDlg->g_bPatchWeld && !g_PrefsDlg->patch_drill_down )
        return;

    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        if ( !b->patch )
            continue;
        patchMesh_t *p = b->patch->def;
        for ( int col = 0; col < p->width; ++col )
        {
            for ( int row = 0; row < p->height; ++row )
            {
                drawVert_t *other = &p->ctrl[col][row];

                if ( g_PrefsDlg->g_bPatchWeld )
                {
                    // weld: identical xyz to the clicked point → queue it (de-dup'd).
                    float d0 = other->xyz[0] - cp->xyz[0];
                    float d1 = other->xyz[1] - cp->xyz[1];
                    float d2 = other->xyz[2] - cp->xyz[2];
                    if ( d0 * d0 <= 1.00000011e-06f && d1 * d1 <= 1.00000011e-06f
                         && d2 * d2 <= 1.00000011e-06f )
                    {
                        // dedup by POINTER: cp itself is already queued, every
                        // distinct coincident seam point gets appended so the weld moves as one.
                        if ( Patch_FindMovePointByPtr( other ) < 0 )
                            Patch_AppendMovePoint( p, other );
                        continue;       // already handled this point
                    }
                }

                if ( g_PrefsDlg->patch_drill_down && g_nPatchClickedView != 1 )
                {
                    // drill-down: same on-screen position (ignoring depth) → queue it.
                    float da = (float)fabs( cp->xyz[axisA] - other->xyz[axisA] );
                    if ( da > 0.001f )
                        continue;
                    float db = (float)fabs( cp->xyz[axisB] - other->xyz[axisB] );
                    if ( db > 0.001f )
                        continue;
                    if ( Patch_FindMovePointByPtr( other ) < 0 )      // dedup by POINTER
                        Patch_AppendMovePoint( p, other );
                }
            }
        }
    }
}

// ── Patch_ColumnAllSelected (0x43c380) ─────────────────────────────
// 1 iff every (non-marked, turned_edge bit1 clear) control point in column `col` of
// patch `def` is already queued in d_move_points[].  Drives the click-toggle between
// column-select and row-select.
static char Patch_ColumnAllSelected( patchMesh_t *def, int col )
{
    if ( def->height <= 0 )
        return 1;
    for ( int row = 0; row < def->height; ++row )
    {
        drawVert_t *cp = &def->ctrl[col][row];
        if ( ( cp->turned_edge & 2 ) == 0 )
        {
            if ( Patch_FindMovePointByPtr( cp ) < 0 )      // pointer equality, not xyz
                return 0;
        }
    }
    return 1;
}

// ── SelectColumn (0x43c760) / SelectRow (0x43c710) ────────────────────────────
// Queue every non-marked control point of a whole grid column / row (clearing the
// move-point list first unless `bMulti`).  The shift-modifier multi-select.
static void Patch_SelectColumn( patchMesh_t *def, int col, char bMulti )
{
    if ( !bMulti )
        g_qeglobals.d_num_move_points = 0;
    for ( int row = 0; row < def->height; ++row )
    {
        drawVert_t *cp = &def->ctrl[col][row];
        if ( ( cp->turned_edge & 2 ) == 0 )
            Patch_SelectCtrlPoint( def, cp, 0 );
    }
}

static void Patch_SelectRow( patchMesh_t *def, int row, char bMulti )
{
    if ( !bMulti )
        g_qeglobals.d_num_move_points = 0;
    for ( int col = 0; col < def->width; ++col )
    {
        drawVert_t *cp = &def->ctrl[col][row];
        if ( ( cp->turned_edge & 2 ) == 0 )
            Patch_SelectCtrlPoint( def, cp, 0 );
    }
}

// ── Patch_ShiftCtrlSelectByDelta (0x43C820) ────────────────────────────
// Shift the selected control points of one patch DEF by grid-neighbour delta (dCol,dRow):
// for every cell (col,row) whose NEIGHBOUR (col+dCol, row+dRow) is currently a picked
// move-point (and this cell is not turned-edge-masked, turned_edge & 2), select THIS cell
// (and, unless bAdd, un-pick the neighbour).  The three iteration orders below match the
// binary's three passes chosen so a shifted point is never re-processed within the sweep:
//   dCol<0            : col high→low, row low→high
//   dCol>=0 & dRow>=0 : col low→high, row low→high
//   dCol>=0 & dRow<0  : col low→high, row high→low
// Bounds-checks the neighbour against [0,width)×[0,height).
static void Patch_ShiftCtrlSelectByDelta( patchMesh_t *def, int dCol, int dRow, char bAdd )
{
    const int w = def->width;
    const int h = def->height;

    auto tryCell = [&]( int col, int row )
    {
        int ncol = col + dCol;
        int nrow = row + dRow;
        if ( ncol < 0 || ncol >= w || nrow < 0 || nrow >= h )
            return;
        // Is the neighbour currently a picked move-point?
        drawVert_t *neighbour = &def->ctrl[ncol][nrow];
        int m = 0;
        for ( ; m < g_qeglobals.d_num_move_points; ++m )
            if ( g_qeglobals.d_move_points[m] == neighbour )
                break;
        if ( m >= g_qeglobals.d_num_move_points )
            return;
        drawVert_t *cell = &def->ctrl[col][row];
        if ( ( cell->turned_edge & 2 ) != 0 )
            return;
        if ( !bAdd )
            Patch_RemoveMovePoint( neighbour->xyz );
        Patch_SelectCtrlPoint( def, cell, 0 );
    };

    if ( dCol < 0 )
    {
        for ( int col = w - 1; col >= 0; --col )
            for ( int row = 0; row < h; ++row )
                tryCell( col, row );
    }
    else if ( dRow >= 0 )
    {
        for ( int col = 0; col < w; ++col )
            for ( int row = 0; row < h; ++row )
                tryCell( col, row );
    }
    else
    {
        for ( int col = 0; col < w; ++col )
            for ( int row = h - 1; row >= 0; --row )
                tryCell( col, row );
    }
    g_nUpdateBits = -1;
}

// ── Patch_KeyMoveSelection (0x43CB80) — move the patch-vertex selection by a grid delta ─────────────
// Called by the camera fly keys (Left/Right/Forward/Back).  Only acts in vertex/curve-
// point select mode: for every selected patch, shift its picked control points by
// (dCol,dRow) and return 1 (handled) — so the caller leaves the camera alone.  Returns 0
// (not handled) otherwise, letting the camera move.
char Patch_KeyMoveSelection( char bAdd, int dCol, int dRow )
{
    if ( g_qeglobals.d_select_mode != sel_vertex && g_qeglobals.d_select_mode != sel_curvepoint )
        return 0;

    char handled = 0;
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        if ( b->patch )
        {
            iassert( b->patch->def == b->def->patch );   // PMESH.CPP:2230
            Patch_ShiftCtrlSelectByDelta( b->patch->def, dCol, dRow, bAdd );
            handled = 1;
        }
    }
    return handled;
}

// ── Patch_ClickControlPoint (0x43cc00) ────────────────────────────────────────
// `cp` = the clicked d_points[] slot (a vec3*).  Find the control point in the
// selected patches whose xyz matches it, then apply the modifier action.  The two
// flags come from SelectCurvePointByRay:
//   bMultiAppend  (buttons & MK_CONTROL/8) → APPEND to the selection (don't clear).
//   bColRowSelect (buttons & MK_SHIFT/4)   → select a whole COLUMN, or a ROW once
//                                            the column is fully selected.
//   plain (neither)  → first click in a fresh view records the "same-view" flag and
//                      returns; a repeat click on an already-selected point de-selects
//                      it, else it becomes the sole selection.
// bLockPatchVerts / bUnlockPatchVerts (off by default) force the control point's
// "marked" (turned_edge bit1) flag on/off instead of selecting.
static void Patch_ClickControlPoint( const float *cp, char bMultiAppend, char bColRowSelect )
{
    // First plain click in a fresh view just arms the "same-view" flag (so the NEXT
    // click in the same view can de-select).  g_bXYViewIsLastPatchClick is set by XY_MouseDown.
    if ( !g_bXYViewIsLastPatchClick && !bMultiAppend && !bColRowSelect )
    {
        g_bXYViewIsLastPatchClick = 1;
        return;
    }

    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        if ( !b->patch )
            continue;
        iassert( b->patch->def == b->def->patch );

        patchMesh_t *def = b->def->patch;
        for ( int col = 0; col < def->width; ++col )
        {
            for ( int row = 0; row < def->height; ++row )
            {
                drawVert_t *here = &def->ctrl[col][row];

                // does this control point's xyz match the clicked d_points slot?
                int k = 0;
                for ( ; k < 3; ++k )
                {
                    float d = here->xyz[k] - cp[k];
                    if ( d * d > 1.00000011e-06f )
                        break;
                }
                if ( k != 3 )
                    continue;               // not this control point

                // ── matched the clicked control point ──────────────────────────
                if ( g_qeglobals.bLockPatchVerts )
                {
                    g_qeglobals.d_num_move_points = 0;
                    here->turned_edge |= 2;
                    continue;               // keep scanning (mark every coincident)
                }
                if ( g_qeglobals.bUnlockPatchVerts )
                {
                    g_qeglobals.d_num_move_points = 0;
                    here->turned_edge &= ~2;
                    continue;
                }

                int existing = Patch_FindMovePointByPtr( here );   // pointer equality
                if ( existing < 0 )
                {
                    // not yet selected
                    if ( bColRowSelect )                       // column select
                    {
                        Patch_SelectColumn( def, col, bMultiAppend );
                        return;
                    }
                    if ( !bMultiAppend )                       // replace selection
                        g_qeglobals.d_num_move_points = 0;
                    if ( ( here->turned_edge & 2 ) == 0 )
                        Patch_SelectCtrlPoint( def, here, 1 );
                    return;
                }
                // already selected
                if ( bColRowSelect )                           // column→row toggle
                {
                    if ( Patch_ColumnAllSelected( def, col ) )
                    {
                        Patch_SelectRow( def, row, bMultiAppend );
                        return;
                    }
                    Patch_SelectColumn( def, col, bMultiAppend );
                    return;
                }
                if ( g_bXYViewIsLastPatchClick )
                {
                    Patch_RemoveMovePoint( cp );    // re-click in same view → de-select
                    return;
                }
            }
        }
    }
}

// ── SelectCurvePointByRay (0x495150) — port lives in select.cpp (sibling of
//    SelectVertexByRay); it calls Patch_ClickControlPoint via this bridge.  The
//    args are the two Patch_ClickControlPoint flags: bMultiAppend=(buttons&8), bColRowSelect=(buttons&4). ──
extern "C" void Patch_ClickControlPoint_C( const float *cp, char bMultiAppend, char bColRowSelect )
{
    Patch_ClickControlPoint( cp, bMultiAppend, bColRowSelect );
}

// ── Patch_EditPatch (0x441fd0) — ENTER curve-point edit mode ──────────────────
// Reset the point lists, fill g_qeglobals.d_points[] with every selected patch's
// control-point world positions (so SelectCurvePointByRay can ray-pick them), and
// switch d_select_mode to sel_curvepoint.  Wired from CMainFrame::OnSelectionDragVertices
// (Selection→Drag Vertices, ID 33007) when only patches are selected.
void Patch_EditPatch()
{
    g_qeglobals.d_numpoints              = 0;
    g_qeglobals.d_num_move_points        = 0;
    g_qeglobals.patch_verts_array01_count = 0;
    g_qeglobals.patch_verts_array02_count = 0;

    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        if ( !b->patch )
            continue;
        iassert( b->patch->def == b->def->patch );  // 4351

        patchMesh_t *def = b->def->patch;
        for ( int col = 0; col < def->width; ++col )
        {
            for ( int row = 0; row < def->height; ++row )
            {
                int idx = g_qeglobals.d_numpoints;
                const float *src = def->ctrl[col][row].xyz;
                g_qeglobals.d_points[idx][0] = src[0];
                g_qeglobals.d_points[idx][1] = src[1];
                g_qeglobals.d_points[idx][2] = src[2];
                if ( g_qeglobals.d_numpoints < 2047 )
                    ++g_qeglobals.d_numpoints;
            }
        }
    }

    select_t prev = g_qeglobals.d_select_mode;
    g_qeglobals.d_select_mode = sel_curvepoint;
    if ( prev == sel_cycle_edge_direction_quad )
        CMainFrame_UpdatePatchToolbarButtons();
}

// ── Patch texture follow ─────────────────────────────────────────────────────────────────
//  Texture lock OFF: a moved control point's three S/T pairs are re-solved so the texture
//  stays put in world space.  Each triangle around the point gives the S/T gradient over its
//  plane (solved from the drag-start savedTexCoord); the gradients of the triangles that use
//  the point are summed, each channel's sum clamped to length 0.25 (the binary's guard against
//  a degenerate quad), and  st = saved + ( sum . delta ) / triangleCount.
//  KIWI: shared by the classic curve-point drag and the G vertex move (kiwi_transform.cpp);
//  the gradients accumulate in double.
extern void Ed_Normal_Calc( const float *normal, float *xv, float *yv );            // texturevecs.cpp 0x459A60
extern char Ed_Patch_AffineInverse( const float *xs, const float *ys, double *inv );  // brush.cpp 0x43CEE0
int         PlaneFromPoints_Real( float *out, const float *A, const float *B, const float *C );   // below

// 0x43d240 + 0x43cff0: add one triangle's gradients to grad (per layer 8 values: s gradient,
// s constant, t gradient, t constant).  false (nothing added) for a degenerate triangle.
static bool Patch_TexFollow_TriGradient( const patchMesh_t *p, int r0, int c0, int r1, int c1,
                                         int r2, int c2, double grad[24] )
{
    const drawVert_t *v[3] = { &p->ctrl[c0][r0], &p->ctrl[c1][r1], &p->ctrl[c2][r2] };
    float plane[4];
    if ( !PlaneFromPoints_Real( plane, v[1]->xyz, v[0]->xyz, v[2]->xyz ) )
        return false;
    float xv[3], yv[3];
    Ed_Normal_Calc( plane, xv, yv );

    float xs[3], ys[3];
    for ( int i = 0; i < 3; ++i )
    {
        xs[i] = v[i]->xyz[0] * xv[0] + v[i]->xyz[1] * xv[1] + v[i]->xyz[2] * xv[2];
        ys[i] = v[i]->xyz[0] * yv[0] + v[i]->xyz[1] * yv[1] + v[i]->xyz[2] * yv[2];
    }
    double inv[9];
    if ( !Ed_Patch_AffineInverse( xs, ys, inv ) )
        return false;

    for ( int k = 0; k < 6; ++k )               // layer = k / 2, s or t = k % 2
    {
        double val[3];
        for ( int i = 0; i < 3; ++i )
            val[i] = ( (const float *)&v[i]->savedTexCoord )[k];
        const double a = inv[0] * val[0] + inv[1] * val[1] + inv[2] * val[2];   // per unit along xv
        const double b = inv[3] * val[0] + inv[4] * val[1] + inv[5] * val[2];   // per unit along yv
        double *g = &grad[4 * k];
        g[0] += a * xv[0] + b * yv[0];
        g[1] += a * xv[1] + b * yv[1];
        g[2] += a * xv[2] + b * yv[2];
        g[3] += inv[6] * val[0] + inv[7] * val[1] + inv[8] * val[2];
    }
    return true;
}

// 0x43d3d0: add the triangles of quad (colSq..colSq+1, rowSq..rowSq+1) that use control point
// (col,row); returns how many.  A terrain quad splits along its corner's turned_edge bit.
static int Patch_TexFollow_QuadGradient( const patchMesh_t *p, int colSq, int rowSq, int col, int row,
                                         double grad[24] )
{
    iassert( row == rowSq || row == rowSq + 1 );   // PMESH.CPP:2458
    iassert( col == colSq || col == colSq + 1 );   // PMESH.CPP:2459
    if ( rowSq < 0 || rowSq + 1 >= p->height || colSq < 0 || colSq + 1 >= p->width )
        return 0;

    int n = 0;
    if ( p->type == PATCH_TERRAIN && ( p->ctrl[colSq][rowSq].turned_edge & 1 ) )
    {
        if ( !( row == rowSq + 1 && col == colSq ) )
            n += Patch_TexFollow_TriGradient( p, rowSq, colSq, rowSq + 1, colSq + 1, rowSq, colSq + 1, grad );
        if ( !( row == rowSq && col == colSq + 1 ) )
            n += Patch_TexFollow_TriGradient( p, rowSq + 1, colSq + 1, rowSq, colSq, rowSq + 1, colSq, grad );
    }
    else
    {
        if ( !( row == rowSq + 1 && col == colSq + 1 ) )
            n += Patch_TexFollow_TriGradient( p, rowSq, colSq, rowSq + 1, colSq, rowSq, colSq + 1, grad );
        if ( !( row == rowSq && col == colSq ) )
            n += Patch_TexFollow_TriGradient( p, rowSq, colSq + 1, rowSq + 1, colSq, rowSq + 1, colSq + 1, grad );
    }
    return n;
}

// 0x43d520: the S/T of control point (col,row) once it has moved `delta` from where its
// savedTexCoord was taken.  false = no usable triangle (leave the S/T as they are).
bool Patch_TexFollow( const patchMesh_t *p, int col, int row, const float *delta, float outST[6] )
{
    double grad[24] = {};
    const int n = Patch_TexFollow_QuadGradient( p, col - 1, row - 1, col, row, grad )
                + Patch_TexFollow_QuadGradient( p, col,     row - 1, col, row, grad )
                + Patch_TexFollow_QuadGradient( p, col - 1, row,     col, row, grad )
                + Patch_TexFollow_QuadGradient( p, col,     row,     col, row, grad );
    if ( n <= 0 )
        return false;

    const float *saved = (const float *)&p->ctrl[col][row].savedTexCoord;
    for ( int k = 0; k < 6; ++k )
    {
        double *g = &grad[4 * k];
        const double len = sqrt( g[0] * g[0] + g[1] * g[1] + g[2] * g[2] );
        if ( len > 0.25 )
        {
            const double s = 0.25 / len;
            g[0] *= s;
            g[1] *= s;
            g[2] *= s;
        }
        const double v = ( g[0] * delta[0] + g[1] * delta[1] + g[2] * delta[2] ) / n + saved[k];
        iassert( isfinite( v ) );   // PMESH.CPP:2528 !IS_NAN(newVal)
        outST[k] = (float)v;
    }
    return true;
}

// ── Patch_UpdateSelected_0 (0x43d800) — APPLY the drag delta + re-tessellate ──
// Add the world delta `move` to every selected control point, rebuild d_points[] from
// the new control positions, and re-tessellate each selected patch (Patch_Rebuild →
// Patch_GenericMesh2) so the curve re-bends live.  `total` is the move since the press
// (Drag_MouseMoved's drag_vec), which the texture follow measures from.
static void Patch_UpdateSelected_0( const float *move, const float *total )
{
    // (0) texture lock OFF: every point's new S/T first, then written.  KIWI: solved on the drag-start geometry (the moved points put
    //     back by the move so far, then restored bit for bit), so a long drag lands where one
    //     step would - the binary solved on the pre-step geometry and overshot - and the
    //     classic drag agrees with G.
    if ( !g_PrefsDlg->m_bTextureLock )
    {
        static float s_followST[4096][6];
        static bool  s_followOk[4096];
        static float s_followXyz[4096][3];
        const int n = g_qeglobals.d_num_move_points;
        iassert( n <= 4096 );   // PMESH.CPP:2593
        const float sofar[3] = { total[0] - move[0], total[1] - move[1], total[2] - move[2] };
        for ( int i = 0; i < n; ++i )
        {
            float *xyz = g_qeglobals.d_move_points[i]->xyz;
            memcpy( s_followXyz[i], xyz, sizeof( s_followXyz[i] ) );
            xyz[0] -= sofar[0];
            xyz[1] -= sofar[1];
            xyz[2] -= sofar[2];
        }
        for ( int i = 0; i < n; ++i )
        {
            patchMesh_t *def;
            int row, col;
            s_followOk[i] = Patch_LocateCtrlPoint( g_qeglobals.d_move_points[i], &def, &row, &col )
                         && Patch_TexFollow( def, col, row, total, s_followST[i] );
        }
        for ( int i = n - 1; i >= 0; --i )     // reverse: a listed-twice point ends on its first copy
            memcpy( g_qeglobals.d_move_points[i]->xyz, s_followXyz[i], sizeof( s_followXyz[i] ) );
        for ( int i = 0; i < n; ++i )
            if ( s_followOk[i] )
                memcpy( &g_qeglobals.d_move_points[i]->texCoord, s_followST[i], sizeof( s_followST[i] ) );
    }

    // (1) translate every queued control point by the snapped world delta.
    for ( int i = 0; i < g_qeglobals.d_num_move_points; ++i )
    {
        drawVert_t *mp = g_qeglobals.d_move_points[i];
        mp->xyz[0] += move[0];
        mp->xyz[1] += move[1];
        mp->xyz[2] += move[2];
    }

    // (2) for each selected patch: rebuild d_points[] from ctrl, then re-tessellate.
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        if ( !b->patch )
            continue;
        iassert( b->patch->def == b->def->patch );

        patchMesh_t *def = b->def->patch;
        g_qeglobals.d_numpoints = 0;
        for ( int col = 0; col < def->width; ++col )
        {
            for ( int row = 0; row < def->height; ++row )
            {
                int idx = g_qeglobals.d_numpoints;
                const float *src = def->ctrl[col][row].xyz;
                g_qeglobals.d_points[idx][0] = src[0];
                g_qeglobals.d_points[idx][1] = src[1];
                g_qeglobals.d_points[idx][2] = src[2];
                if ( g_qeglobals.d_numpoints < 2047 )
                    ++g_qeglobals.d_numpoints;
            }
        }
        Patch_Rebuild( def, 1 );
    }
}

// ── Patch_UpdateSelected (0x43ed10) — the dispatcher MoveSelection calls ───────
// A standard control-point translate (Patch_UpdateSelected_0).  KIWI: the binary's paint-mode-1
// soft-selection height push went with the AdvPatchEditDlg paint path.
// `move` is the snapped world delta (a float[3]); `total` (the move since the press) is for
// the texture follow.
void Patch_UpdateSelected( float *move, const float *total )
{
    Patch_UpdateSelected_0( move, total );
}

// ══════════════════════════════════════════════════════════════════════════════
//  Patch_SaveLayerMtl (0x44cfb0) — the SurfaceInspector's patch-texturing GET.  KIWI: its SET
//  (Patch_RestoreLayerMtl 0x44d0c0) went with the MFC inspector's revert; what remains is the
//  drag-start savedTexCoord snapshot the texture follow (Patch_TexFollow) reads - taken by
//  Patch_RedispersePreDrag and the G vertex move.
//
//  Layout facts (qe3.h): patchMesh_t.texture/lightmap/smoothing are three contiguous
//  patchMesh_material {lyrMtl,radMtl} → indexable as (&p->texture)[layer].  The
//  "live/working" material slot is workingMaterial = one more {lyrMtl,radMtl} pair.
//  xx21 is a byte shadow of bDirty.  Each drawVert_t ctrl point: texCoord live,
//  savedTexCoord snapshot — both pmesh_texcoord{st,lightmap,smoothing} = 3 layers × vec2.
// ══════════════════════════════════════════════════════════════════════════════

// Patch "GET": pull the CURRENT layer's material pointers into the live working slot, then
// SNAPSHOT every control point's live texCoord (all 3 layers) into its savedTexCoord.
//   * live{lyrMtl,radMtl} ← (&p->texture)[current_edit_layer]
//   * xx21 ← bDirty
//   * per ctrl point: savedTexCoord[0..2] ← texCoord[0..2]  (24 bytes)
void Patch_SaveLayerMtl( patchMesh_t *p )
{
    const int layer = g_qeglobals.current_edit_layer;

    // Working material slot ← current layer's material pair.  (&p->texture)[layer] is the
    // layer's {lyrMtl,radMtl}; the live pair is workingMaterial.
    patchMesh_material *live = &p->workingMaterial;
    *live = ( &p->texture )[layer];
    p->xx21 = p->bDirty;

    for ( int row = 0; row < p->height; ++row )          // height is the outer loop
    {
        for ( int col = 0; col < p->width; ++col )
        {
            drawVert_t *v = &p->ctrl[col][row];
            // Snapshot all three layer texCoords (st/lightmap/smoothing = 3 × vec2).
            for ( int l = 0; l < 3; ++l )
            {
                float *dst = &( (float *)&v->savedTexCoord )[2 * l];
                dst[0] = ( (float *)&v->texCoord )[2 * l + 0];
                dst[1] = ( (float *)&v->texCoord )[2 * l + 1];
                iassert( isfinite( dst[0] ) );
                iassert( isfinite( dst[1] ) );
            }
        }
    }
}

// ── Patch_RedispersePreDrag (0x43d7a0) — drag-START hook ──
// Snapshot every touched patch's live texCoords into savedTexCoord before the drag begins
// (Patch_SaveLayerMtl), so the curve-drag edits work off a clean baseline.  Dedupes consecutive
// ctrl points belonging to the same patch (the binary's `if (def != last)` guard).
void Patch_RedispersePreDrag()
{
    patchMesh_t *last = nullptr;
    for ( int i = 0; i < g_qeglobals.d_num_move_points; ++i )
    {
        patchMesh_t *def; int row, col;
        if ( Patch_LocateCtrlPoint( g_qeglobals.d_move_points[i], &def, &row, &col ) )
        {
            if ( def != last )
            {
                Patch_SaveLayerMtl( def );
                last = def;
            }
        }
    }
}

// ── Patch_PaintFinish (0x43ec10) — paint/drag undo-id stamp ───────────────────
//  After a paint or soft-sel drag, for every touched patch brush in the list (paint-dirty
//  flag xx22b set): clear the flag and stamp the current undo record's id onto the brush def
//  (ownerPrev) + its entity (epairEdits) — the same idiom as Undo_LinkBrush, so the stroke
//  participates correctly in undo/redo grouping.  Also the KIWI Terrain Sculpt / Decals
//  stroke release (kiwi_terrain.cpp, kiwi_decal.cpp).
void Patch_PaintFinish( selbrush_t *list )
{
    for ( selbrush_t *i = list->next; i != list; i = i->next )
    {
        patch_t *patch = i->patch;
        if ( !patch )
            continue;
        iassert( patch->def == i->def->patch );
        if ( !patch->def->xx22b )
            continue;
        patch->def->xx22b = 0;
        if ( g_lastundo && !g_lastundo->done )
        {
            i->def->ownerPrev = (entity_s *)(intptr_t)g_lastundo->id;
            entity_s *owner = (entity_s *)(intptr_t)i->def->owner;
            if ( *(int *)&owner->eclass->fixedsize )
                owner->epairEdits = g_lastundo->id;
        }
    }
}

// ── Patch_FlipTexture (0x445e30) — flip a patch's S or T texcoords ───
//  The patch branch of Brush_FlipTexture (Select_FlipTexture, select.cpp).  axis=0
//  flips S, axis=1 flips T.  For each control point — every one outside curve-point
//  mode, only the SELECTED move-points inside it — replaces the current edit layer's
//  st[axis] with 1-st[axis].  Then (layer 1 only) clears bDirty and rebuilds the
//  tessellated curveDef.  Touches editor texCoord state only; never serialized.
curvePatchDef_t *Patch_FlipTexture( patchMesh_t *p, int axis )
{
    const int layer = g_qeglobals.current_edit_layer;

    for ( int col = 0; col < p->width; ++col )
    {
        for ( int row = 0; row < p->height; ++row )
        {
            drawVert_t *cp = &p->ctrl[col][row];

            // In curve-point mode only flip control points that are queued as move-points.
            if ( g_qeglobals.d_select_mode == sel_curvepoint )
            {
                int k = 0;
                for ( ; k < g_qeglobals.d_num_move_points; ++k )
                    if ( (void *)cp == (void *)g_qeglobals.d_move_points[k] )
                        break;
                if ( k >= g_qeglobals.d_num_move_points )
                    continue;
            }

            float *st = (float *)&cp->texCoord;
            float  nv = 1.0f - st[2 * layer + axis];
            iassert( isfinite( nv ) );
            st[2 * layer + axis] = nv;
        }
    }

    if ( layer == 1 )
        p->bDirty = 0;
    if ( p->curveDef )
        free( p->curveDef );
    curvePatchDef_t *result = Patch_GenericMesh2( p, layer, 0, 0 );
    ++p->version;
    p->curveDef = result;
    return result;
}

// ════════════════════════════════════════════════════════════════════════════
//  TERRAIN-EDGE-TURN PICKER GROUP - the "cycle edge direction" (terrain quad-diagonal
//  turn) tool, reached from drag.cpp's sel_cycle_edge_direction_quad mode:
//  XY_MouseDown -> Patch_TurnEdge -> Patch_RayTest (the ray-vs-curve-edge picker), which needs
//  the tessellated curveDef.  For a PATCH_TERRAIN patch the curveDef render mesh IS the
//  control grid (1:1 copy in Patch_GenericMesh2's terrain branch), so verts[col+row*width]
//  aligns with ctrl[col][row].  Nothing here is serialized.
// ════════════════════════════════════════════════════════════════════════════

// ── PlaneFromPoints_Real (0x4a9950) ───────────────────────────────────────────
//  out[0..2] = normalized normal of triangle (A,B,C) about pivot B = (C-B)×(A-B);
//  out[3] = dot(normal, B).  Returns 0 if the triangle is degenerate (both the
//  primary and the alternate-pivot cross collapse), 1 otherwise.  Includes the
//  area-vs-length² degeneracy guard (epsilon 1.00000011e-06).
// non-static: also called by the model ray-pick (Model_RayTest) in select.cpp.
int PlaneFromPoints_Real( float *out, const float *A, const float *B, const float *C )
{
    float e1[3], e2[3];
    e1[0] = A[0] - B[0]; e1[1] = A[1] - B[1]; e1[2] = A[2] - B[2];   // A-B
    e2[0] = C[0] - B[0]; e2[1] = C[1] - B[1]; e2[2] = C[2] - B[2];   // C-B
    Vec3Cross( e2, e1, out );                                       // out = (C-B)×(A-B)

    float lenSq = out[0]*out[0] + out[1]*out[1] + out[2]*out[2];
    if ( lenSq < 2.0f )
    {
        if ( lenSq == 0.0f )
            return 0;
        float l1 = e1[0]*e1[0] + e1[1]*e1[1] + e1[2]*e1[2];
        float l2 = e2[0]*e2[0] + e2[1]*e2[1] + e2[2]*e2[2];
        if ( (float)( l1 * l2 * 0.00000100000011116208 ) >= lenSq )
        {
            // alternate pivot A: normal = (C-A)×(B-A)
            float f1[3], f2[3];
            f1[0] = C[0] - A[0]; f1[1] = C[1] - A[1]; f1[2] = C[2] - A[2];
            f2[0] = B[0] - A[0]; f2[1] = B[1] - A[1]; f2[2] = B[2] - A[2];
            Vec3Cross( f2, f1, out );
            float m1 = f1[0]*f1[0] + f1[1]*f1[1] + f1[2]*f1[2];
            float m2 = f2[0]*f2[0] + f2[1]*f2[1] + f2[2]*f2[2];
            if ( (float)( m1 * m2 * 0.00000100000011116208 ) >= lenSq )
                return 0;
        }
    }
    float len = (float)sqrt( lenSq );
    out[0] /= len; out[1] /= len; out[2] /= len;
    out[3] = out[2]*B[2] + out[0]*B[0] + out[1]*B[1];
    return 1;
}

// ── Ray_TriangleHit (0x44ab10) — ray-vs-triangle pick ────────────
//  Möller–Trumbore intersection of ray (origin `org`, direction `dir`) against the
//  triangle (base, vA, vB).  edge1 = vA-base, edge2 = vB-base.  On a hit returns 1
//  and writes the ray parameter to *outT; *outU/*outV (when non-null) receive the
//  barycentric coords.  Two-sided (sign tracks the winding).  Determinant epsilon
//  1e-4, as in the binary.
// non-static: also called by the model ray-pick (Model_RayTest) in select.cpp.
char Ray_TriangleHit( const float *vB, const float *vA, const float *dir,
                              const float *base, const float *org,
                              float *outT, float *outU, float *outV )
{
    float edge1[3], edge2[3], pvec[3], tvec[3], qvec[3];
    edge1[0] = vA[0] - base[0]; edge1[1] = vA[1] - base[1]; edge1[2] = vA[2] - base[2];
    edge2[0] = vB[0] - base[0]; edge2[1] = vB[1] - base[1]; edge2[2] = vB[2] - base[2];
    Vec3Cross( dir, edge2, pvec );

    float det = edge1[0]*pvec[0] + edge1[1]*pvec[1] + edge1[2]*pvec[2];
    float sign;
    if ( det >= 0.0001 )       sign = 1.0f;
    else if ( det > -0.0001 )  return 0;
    else { sign = -1.0f; det = -det; }

    tvec[0] = org[0] - base[0]; tvec[1] = org[1] - base[1]; tvec[2] = org[2] - base[2];
    float u = ( pvec[0]*tvec[0] + pvec[1]*tvec[1] + pvec[2]*tvec[2] ) * sign;
    if ( u < 0.0f || u > det )
        return 0;

    Vec3Cross( tvec, edge1, qvec );
    float v = ( dir[0]*qvec[0] + dir[1]*qvec[1] + dir[2]*qvec[2] ) * sign;
    if ( v < 0.0f || det < u + v )
        return 0;

    float t = ( edge2[0]*qvec[0] + edge2[1]*qvec[1] + edge2[2]*qvec[2] ) * sign / det;
    *outT = t;
    if ( outU ) *outU = u / det;
    if ( outV ) *outV = v / det;
    return 1;
}

// barycentric blend of three curveVerts' RGBA bytes: base*w + A*u + B*v (w=1-u-v).
static void CurveVert_BlendColor( const curveVert_t *base, const curveVert_t *A,
                                    const curveVert_t *B, float u, float v, float *outRGBA )
{
    double w = 1.0 - u - v;
    const byte *cb = (const byte *)&base->vert_color;
    const byte *ca = (const byte *)&A->vert_color;
    const byte *cc = (const byte *)&B->vert_color;
    for ( int k = 0; k < 4; ++k )
        outRGBA[k] = (float)( (double)ca[k] * u + (double)cc[k] * v + (double)cb[k] * w );
}

// ── Patch_RayTest (0x44acc0) — ray-vs-curve-edge picker ────────────────────────────
//  Walk the tessellated curveDef grid, split each quad into two triangles along the
//  cell diagonal (turned for PATCH_TERRAIN cells with turned_edge&1), and ray-test
//  each.  Returns the nearest hit (dist > 2.0): *outDist = parameter, *outCol/*outRow
//  = the cell, *outColor (4 bytes, when non-null) = the barycentric vertex colour,
//  *outPlane (vec4, when non-null) = the hit triangle's plane.
// non-static: also called by the per-brush ray trace (Brush_TraceTest) in select.cpp.
char Patch_RayTest( const float *org, const float *dir, patch_t *pm,
                      float *outDist, int *outCol, int *outRow,
                      byte *outColor, float *outPlane )
{
    iassert( pm );          // 8042
    iassert( pm->def );     // 8043

    curvePatchDef_t *cd = pm->def->curveDef;
    if ( !cd )
        return 0;

    const int   width  = cd->width;
    const int   isTerr = ( pm->def->type & PATCH_TERRAIN ) != 0;
    float       best   = 3.4028235e38f;
    char        found  = 0;
    int         bestCol = 0, bestRow = 0;
    float       color[4] = { 0, 0, 0, 0 };

    for ( int row = 0; row < cd->height - 1; ++row )
    {
        for ( int col = 0; col < width - 1; ++col )
        {
            const int idx = col + row * width;
            curveVert_t *verts = cd->verts;
            curveVert_t *base, *vA, *vB2, *vDiag;   // vDiag = the far end of the cell diagonal

            if ( isTerr && ( pm->def->ctrl[col][row].turned_edge & 1 ) )
            {
                base  = &verts[idx];              // TL
                vA    = &verts[idx + 1];          // TR
                vDiag = &verts[idx + width + 1];  // BR  (turned diagonal TL–BR)
                vB2   = &verts[idx + width];      // BL
            }
            else
            {
                vA    = &verts[idx];              // TL
                base  = &verts[idx + width];      // BL
                vDiag = &verts[idx + 1];          // TR  (diagonal BL–TR)
                vB2   = &verts[idx + width + 1];  // BR
            }

            // Triangle #1: (base, vA, vDiag), tested both windings (swap A/B and u/v).
            float dist, bu, bv;
            if ( Ray_TriangleHit( vDiag->xyz, vA->xyz, dir, base->xyz, org, &dist, &bu, &bv )
              || Ray_TriangleHit( vA->xyz, vDiag->xyz, dir, base->xyz, org, &dist, &bv, &bu ) )
            {
                if ( dist > 2.0f )
                {
                    found = 1;
                    if ( best > dist )
                    {
                        if ( outPlane )
                        {
                            float pl[4];
                            PlaneFromPoints_Real( pl, base->xyz, vA->xyz, vDiag->xyz );
                            outPlane[0] = pl[0]; outPlane[1] = pl[1]; outPlane[2] = pl[2];
                        }
                        best = dist;
                        bestCol = col;
                        bestRow = row;
                        CurveVert_BlendColor( base, vA, vDiag, bu, bv, color );
                    }
                }
            }

            // Triangle #2: (base, vDiag, vB2), tested both windings.
            if ( Ray_TriangleHit( vB2->xyz, vDiag->xyz, dir, base->xyz, org, &dist, &bu, &bv )
              || Ray_TriangleHit( vDiag->xyz, vB2->xyz, dir, base->xyz, org, &dist, &bv, &bu ) )
            {
                if ( dist > 2.0f )
                {
                    found = 1;
                    if ( best > dist )
                    {
                        if ( outPlane )
                        {
                            float pl[4];
                            PlaneFromPoints_Real( pl, base->xyz, vDiag->xyz, vB2->xyz );
                            outPlane[0] = pl[0]; outPlane[1] = pl[1]; outPlane[2] = pl[2];
                        }
                        best = dist;
                        bestCol = col;
                        bestRow = row;
                        CurveVert_BlendColor( base, vDiag, vB2, bu, bv, color );
                    }
                }
            }
        }
    }

    if ( !found )
        return 0;

    *outDist = best;
    if ( outCol ) *outCol = bestCol;
    if ( outRow ) *outRow = bestRow;
    if ( outColor )
    {
        // KIWI: fistp rounds (0x44B153..0x44B19D); the (int) cast truncated a picked colour.
        outColor[0] = (byte)KiwiRound_Fistp( color[0] );
        outColor[1] = (byte)KiwiRound_Fistp( color[1] );
        outColor[2] = (byte)KiwiRound_Fistp( color[2] );
        outColor[3] = (byte)KiwiRound_Fistp( color[3] );
    }
    return found;
}

// KIWI: the AdvPatchEditDlg terrain-paint core that sat here (Patch_PaintPick, the paint-mode
// callbacks and 4D value noise, the CurvEditDlg slot table, the cursor-ring clip, the stroke /
// soft-drag apply) went with the paint path; Terrain Sculpt (kiwi_terrain.cpp) replaced it.

// PointToBoundsDistSq (0x4A56B0) — squared distance from a point to an AABB (0 when inside).
// Also camwnd.cpp's light-region shadow-caster gather.
float PointToBoundsDistSq( const float *center, const float *mins, const float *maxs )
{
    float d2 = 0.0f;
    for ( int i = 0; i < 3; ++i )
    {
        const float lo = mins[i] - center[i];
        if ( lo > 0.0f ) { d2 += lo * lo; continue; }   // center < mins -> (mins-center)^2
        const float hi = center[i] - maxs[i];
        if ( hi > 0.0f )   d2 += hi * hi;                // center > maxs -> (center-maxs)^2
    }                                                    // else inside this axis -> 0
    return d2;
}

// Undo_AddPaintedBrush (0x45E770) — register a painted patch with the current undo record
//  (add the owning entity too when it is fixed-size), so a paint stroke is undoable.
//  (Terrain Sculpt's Patch_PaintMarkUndo, below.)
static void Undo_AddPaintedBrush( entity_brush_s *pBrushInst )
{
    if ( !g_lastundo )
    {
        Sys_Printf( "Undo_AddBrush: no last undo.\n" );
        return;
    }
    if ( g_lastundo->entitylist.next != &g_lastundo->entitylist )
        Sys_Printf( "Undo_AddBrush: WARNING adding brushes after entity.\n" );

    entity_s *owner = pBrushInst->owner;
    if ( *(int *)&owner->eclass->fixedsize )           // fixed-size entity -> also track the entity
        Undo_AddEntity( (entity_s *)owner );
    Undo_AddBrush( pBrushInst );
}

// ── Patch_TurnEdge (0x44b4c0) — flip a terrain quad's diagonal ────────────────
//  Ray-pick the nearest terrain edge across all selected patches (Patch_RayTest), then
//  toggle that cell's turned_edge diagonal and rebuild.  Undo-bracketed.  `org`/`dir`
//  arrive as int-cast float* ray endpoints (drag.cpp's trace_start / trace_dir).
void Patch_TurnEdge( float *org, float *dir )
{
    const float *rayOrg = (const float *)(intptr_t)org;
    const float *rayDir = (const float *)(intptr_t)dir;

    float        best = 3.4028235e38f;
    patchMesh_t *hitDef = nullptr;
    int          hitCol = 0, hitRow = 0;

    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        patch_t *patch = b->patch;
        if ( !patch )
            continue;
        if ( patch->def->type != PATCH_TERRAIN )
            continue;

        float dist;
        int   col, row;
        if ( Patch_RayTest( rayOrg, rayDir, patch, &dist, &col, &row, nullptr, nullptr ) )
        {
            if ( dist < best )
            {
                best   = dist;
                hitCol = col;
                hitDef = b->patch->def;
                hitRow = row;
            }
        }
    }

    if ( !hitDef )
        return;

    Undo_ClearRedo();
    Undo_GeneralStart( "Turn terrain edge" );

    entity_brush_s *pSymbiot = hitDef->pSymbiot;
    if ( g_lastundo )
    {
        if ( g_lastundo->entitylist.next != &g_lastundo->entitylist )
            Sys_Printf( "Undo_AddBrush: WARNING adding brushes after entity.\n" );
        entity_s *owner = pSymbiot->owner;
        if ( *(int *)&owner->eclass->fixedsize )
            Undo_AddEntity( (entity_s *)owner );
        Undo_AddBrush( pSymbiot );
    }
    else
    {
        Sys_Printf( "Undo_AddBrush: no last undo.\n" );
    }

    hitDef->ctrl[hitCol][hitRow].turned_edge ^= 1u;
    Patch_Rebuild( hitDef, 1 );

    g_nUpdateBits = -1;

    // Re-stamp the (possibly rebuilt) symbiont with the open undo id — Undo_EndBrushList
    // body for the single brush (Patch_Rebuild can swap pSymbiot, so reload it).
    pSymbiot = hitDef->pSymbiot;
    if ( g_lastundo && !g_lastundo->done )
    {
        pSymbiot->ownerPrev = (selbrush_t *)(intptr_t)g_lastundo->id;
        entity_s *owner = pSymbiot->owner;
        if ( *(int *)&owner->eclass->fixedsize )
            owner->epairEdits = g_lastundo->id;
    }

    Undo_End();
}

// ════════════════════════════════════════════════════════════════════════════
//  PATCH REDISPERSE (ins/del) MODE.  KIWI: the classic Curve->Bend mode (Patch_BendToggle /
//  _HandleTAB / _HandleEnter / Patch_SelectBendAxis / _SelectBendNormal /
//  Patch_SetBendRotateOrigin, 0x4478e0..0x447d30, and its drag / 2D-view arms) is gone - it had
//  no key, menu or palette row and its prompts went nowhere; G on control points and Terrain
//  Sculpt edit patches.  Recover from git: `git show HEAD:src/radiant/pmesh.cpp`.
// ════════════════════════════════════════════════════════════════════════════

// patchdialog.cpp — the modeless "Information" state prompt (0x40BE90) + its messages.
extern void        ShowInfoDialog( const char *msg );
extern void        HideInfoDialog();
extern const char *g_pInsDelStateMsg;                       // 0x73B128

// Redisperse-mode state (defined here — pmesh is the patch home).
char g_bPatchAxisOnRow = 0;     // 0x739B0D  the axis runs along a ROW (else a column)
int  g_nPatchAxisIndex = 0;     // 0x25D5B0C row/column index of the axis
int  g_nPatchInsertState = 0;   // 0x73B110  redisperse (ins/del) mode state machine


// ── Patch_InsDelToggle (0x447e50) — enter/leave the "redisperse control points" mode ──
//  Toggle OFF: clear g_qeglobals_redispersePatchVerts, refresh the patch toolbar.
//  Toggle ON: requires a single selected patch; snapshots it (patchSave_*) for the CANCEL
//  path, arms the redisperse state machine (insert-state 0, axis = row 0), and shows the
//  ins/del info prompt.  Reached from OnPatchRedisperse (toolbar 32872) and the Curve menu.
void Patch_InsDelToggle()
{
    if ( g_qeglobals_redispersePatchVerts )
    {
        g_qeglobals_redispersePatchVerts = 0;
        HideInfoDialog();
        CMainFrame_UpdatePatchToolbarButtons();
    }
    else
    {
        patch_t *patch;
        patchMesh_t *def;
        if ( QE_SingleBrush()
             && ( patch = selected_brushes.next->patch ) != nullptr
             && ( def = patch->def ) != nullptr )
        {
            patchSave_width  = def->width;
            patchSave_height = def->height;
            patchSave_type   = def->type;
            memcpy( patchSave_ctrl, def->ctrl, 0x5000u );
            g_qeglobals_redispersePatchVerts = 1;
            g_nPatchInsertState = 0;
            g_bPatchAxisOnRow   = 1;
            g_nPatchAxisIndex   = 0;
            ShowInfoDialog( g_pInsDelStateMsg );
        }
        else
        {
            Sys_Printf( "Must work with a single patch" );
        }
    }
}

// ── Patch_InsDelHandleTAB (0x447f40) — TAB inside REDISPERSE (ins/del) mode ────
//  Not in redisperse mode -> the TAB simply toggles the mode on.  Otherwise step the
//  axis index by +2 and wrap across the row<->column boundary at height-1 / width-1.
void Patch_InsDelHandleTAB()
{
    if ( !g_qeglobals_redispersePatchVerts )
    {
        Patch_InsDelToggle();
        return;
    }

    patch_t *patch;
    patchMesh_t *def;
    if ( !QE_SingleBrush()
         || ( patch = selected_brushes.next->patch ) == nullptr
         || ( def = patch->def ) == nullptr )
    {
        // The binary also ran Patch_BendToggle here, which only printed "Must bend a single
        // patch" with no single patch selected (KIWI: bend mode is gone).
        Sys_Printf( "No patch to bend!" );
        return;
    }

    const int idx = g_nPatchAxisIndex + 2;
    g_nPatchAxisIndex = idx;
    if ( g_bPatchAxisOnRow )
    {
        if ( idx >= def->height - 1 ) { g_bPatchAxisOnRow = 0; g_nPatchAxisIndex = 0; }
    }
    else if ( idx >= def->width - 1 )
    {
        g_bPatchAxisOnRow = 1;
        g_nPatchAxisIndex = 0;
    }
    g_nUpdateBits = -1;
}

// ── Patch_Deselect (0x442610) — leave every patch-edit mode ───────────────────
//  Called from Select_Deselect.  Resets the edit mode to sel_brush (closing the point
//  edit that owned the old mode: quad-cycle refreshes the toolbar, addpoint finishes the
//  curve drag), clears the per-instance selected flag on every selected patch, and drops
//  redisperse mode if it is armed (the binary also dropped bend mode).
void Patch_Deselect()
{
    const select_t prevMode = g_qeglobals.d_select_mode;
    g_qeglobals.d_select_mode = sel_brush;
    if ( prevMode == sel_cycle_edge_direction_quad )
        CMainFrame_UpdatePatchToolbarButtons();

    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
        if ( b->patch )
            b->patch->selected = 0;

    if ( g_qeglobals_redispersePatchVerts )
        Patch_InsDelToggle();
}

// ════════════════════════════════════════════════════════════════════════════
//  TERRAIN PAINT undo marking.  Patch_Paint clears each patch's "painting-active" flag
//  (xx22b) so a stroke can re-mark touched cells (Terrain Sculpt / Decals).
// ════════════════════════════════════════════════════════════════════════════

// ── Patch_Paint (0x43eb70) — clear the paint-active flag on every patch in a list ─
void Patch_Paint( selbrush_t *list )
{
    for ( selbrush_t *b = list->next; b != list; b = b->next )
    {
        patch_t *patch = b->patch;
        if ( patch )
        {
            iassert( b->patch->def == b->def->patch );  // 3109
            patch->def->xx22b = 0;
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  ADAPTIVE texCoord RESAMPLER
//  The "set patch sample size" texturing: re-distribute a patch's S/T across an
//  arc-length-ADAPTIVE subdivision so densely-curved regions get proportionally more
//  texture space.  Two outcomes: a near-planar patch projects its texture onto the
//  fitted plane (Curve_FitPlane → Patch_PlanarTexProject); a curved one spreads S/T by
//  the adaptive subdivision's index maps (Curve_AdaptiveSubdivide).  Terrain patches
//  project onto the dominant axis instead (Patch_TerrainTexProject).  All editor-only,
//  display-only (texCoords are recomputed on parse) — no .map round-trip impact.
// ════════════════════════════════════════════════════════════════════════════

extern int Vec3_MajorAxis( const float *dir );   // linearmapping.cpp 0x4A45D0 — dominant-axis index

// ── Curve_LerpVert (0x433750) — linear-interpolate one curveVert (a→b by t) ─
static void Curve_LerpVert( curveVert_t *dst, const curveVert_t *a, const curveVert_t *b, float t )
{
    const float *fa = (const float *)a;
    const float *fb = (const float *)b;
    float       *fd = (float *)dst;
    // xyz(0..2) + st(3..4) + lightmap(5..6) = the first 7 floats (bytes 0..24).
    for ( int k = 0; k < 7; ++k )
        fd[k] = ( fb[k] - fa[k] ) * t + fa[k];
    // vert_color rgba (bytes 40..43): lerped in double then truncated to a byte.
    const byte *ba = (const byte *)a;
    const byte *bb = (const byte *)b;
    byte       *bd = (byte *)dst;
    for ( int k = 40; k < 44; ++k )
        bd[k] = (byte)(int)( (double)( (int)bb[k] - (int)ba[k] ) * t + (double)ba[k] );
    // normal (floats 7..9, bytes 28..36), then renormalize.
    fd[7] = ( fb[7] - fa[7] ) * t + fa[7];
    fd[8] = ( fb[8] - fa[8] ) * t + fa[8];
    fd[9] = t * ( fb[9] - fa[9] ) + fa[9];
    Vec3Normalize_R( &fd[7] );
}

// ── Curve_AdaptiveSubdivide (0x4338E0) — arc-length adaptive re-subdivision ──
//  Subdivide each column gap (then each row gap) by max-edge-length / sampleSize,
//  capped so the result never exceeds the 512×512 scratch.  Fills colMap/rowMap with,
//  for each refined index, the original control index it came from (so the resampler can
//  map control points → refined positions).  Clean 2-D indexing into s_curveScratch;
//  numerically identical to the binary's byte-shuffling.
static curvePatchDef_t *Curve_AdaptiveSubdivide( const curvePatchDef_t *in, float sampleSize,
                                                 int *colCounts, int *rowCounts,
                                                 int *colMap, int *rowMap )
{
    const int origW = in->width;
    const int origH = in->height;

    int W = origW, H = origH;
    for ( int col = 0; col < origW; ++col )
        for ( int row = 0; row < origH; ++row )
            s_curveScratch[row][col] = in->verts[col + row * origW];

    // PHASE 1 — subdivide columns (max edge length is taken down each column gap).
    const int capCol = ( origW > 1 ) ? ( 512 - origW ) / ( origW - 1 ) : 0;
    int outCol = 0;
    for ( int gap = 0; gap < origW - 1; ++gap )
    {
        float maxLen = 0.0f;
        for ( int row = 0; row < H; ++row )
        {
            const float *a = s_curveScratch[row][outCol].xyz;
            const float *b = s_curveScratch[row][outCol + 1].xyz;
            float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
            float len = (float)sqrt( dx * dx + dy * dy + dz * dz );
            if ( maxLen < len ) maxLen = len;
        }
        int n = (int)( maxLen / sampleSize );
        if ( n > capCol ) n = capCol;
        colCounts[gap] = n + 1;
        if ( n > 0 )
        {
            W += n;
            if ( colMap )
            {
                for ( int q = W - 1; q >= n + outCol; --q ) colMap[q] = colMap[q - n];
                for ( int q = 0; q < n; ++q )               colMap[outCol + 1 + q] = colMap[outCol];
            }
            for ( int row = 0; row < H; ++row )
            {
                for ( int q = W - 1; q > outCol + n; --q )
                    s_curveScratch[row][q] = s_curveScratch[row][q - n];
                for ( int s = 1; s <= n; ++s )
                {
                    float t = (float)( (double)s / (double)( n + 1 ) );
                    Curve_LerpVert( &s_curveScratch[row][outCol + s],
                                    &s_curveScratch[row][outCol],
                                    &s_curveScratch[row][outCol + n + 1], t );
                }
            }
        }
        outCol += n + 1;
    }

    // PHASE 2 — subdivide rows (max edge length is taken across each row gap).
    const int capRow = ( origH > 1 ) ? ( 512 - origH ) / ( origH - 1 ) : 0;
    int outRow = 0;
    for ( int gap = 0; gap < origH - 1; ++gap )
    {
        float maxLen = 0.0f;
        for ( int col = 0; col < W; ++col )
        {
            const float *a = s_curveScratch[outRow][col].xyz;
            const float *b = s_curveScratch[outRow + 1][col].xyz;
            float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
            float len = (float)sqrt( dx * dx + dy * dy + dz * dz );
            if ( maxLen < len ) maxLen = len;
        }
        int n = (int)( maxLen / sampleSize );
        if ( n > capRow ) n = capRow;
        rowCounts[gap] = n + 1;
        if ( n > 0 )
        {
            H += n;
            if ( rowMap )
            {
                for ( int q = H - 1; q >= n + outRow; --q ) rowMap[q] = rowMap[q - n];
                for ( int q = 0; q < n; ++q )               rowMap[outRow + 1 + q] = rowMap[outRow];
            }
            for ( int col = 0; col < W; ++col )
            {
                for ( int q = H - 1; q > outRow + n; --q )
                    s_curveScratch[q][col] = s_curveScratch[q - n][col];
                for ( int s = 1; s <= n; ++s )
                {
                    float t = (float)( (double)s / (double)( n + 1 ) );
                    Curve_LerpVert( &s_curveScratch[outRow + s][col],
                                    &s_curveScratch[outRow][col],
                                    &s_curveScratch[outRow + n + 1][col], t );
                }
            }
        }
        outRow += n + 1;
    }

    size_t bytes = (size_t)44 * W * H;
    curvePatchDef_t *out = (curvePatchDef_t *)malloc( bytes + sizeof(curvePatchDef_t) );
    out->width  = W;
    out->height = H;
    out->verts  = (curveVert_t *)( out + 1 );
    for ( int row = 0; row < H; ++row )
        for ( int col = 0; col < W; ++col )
            out->verts[col + row * W] = s_curveScratch[row][col];
    return out;
}

// ── Curve_FitPlane (0x432690) — is the tessellated mesh planar (±1)? ─────────
//  Sum the per-cell triangle normals + corner positions; if the summed normal is
//  non-degenerate AND every vertex lies within ±1 of the fitted plane, return 1 + the
//  plane (normal in out[0..2], dist in out[3]).  Else 0.
static int Curve_FitPlane( const curvePatchDef_t *cd, float *out )
{
    const int w = cd->width;
    const curveVert_t *V = cd->verts;
    float nrm[3] = { 0, 0, 0 };          // summed normal
    float pos[3] = { 0, 0, 0 };          // summed corner
    int   count  = 0;

    for ( int row = 0; row < cd->height - 1; ++row )
    {
        for ( int col = 0; col < w - 1; ++col )
        {
            const float *v00 = V[col + row * w].xyz;
            const float *v10 = V[col + 1 + row * w].xyz;
            const float *v01 = V[col + ( row + 1 ) * w].xyz;
            const float *v11 = V[col + 1 + ( row + 1 ) * w].xyz;

            // T1: edge1 = v10-v00, edge2 = v01-v00 ; accumulate v00.
            float e1[3] = { v10[0] - v00[0], v10[1] - v00[1], v10[2] - v00[2] };
            float e2[3] = { v01[0] - v00[0], v01[1] - v00[1], v01[2] - v00[2] };
            float cr[3];
            Vec3Cross( e1, e2, cr );
            if ( Vec3Normalize_R( cr ) > 0.001f )
            {
                nrm[0] += cr[0]; nrm[1] += cr[1]; nrm[2] += cr[2];
                pos[0] += v00[0]; pos[1] += v00[1]; pos[2] += v00[2];
                ++count;
            }
            // T2: edge1 = v01-v11, edge2 = v10-v11 ; accumulate v11.
            float f1[3] = { v01[0] - v11[0], v01[1] - v11[1], v01[2] - v11[2] };
            float f2[3] = { v10[0] - v11[0], v10[1] - v11[1], v10[2] - v11[2] };
            Vec3Cross( f1, f2, cr );
            if ( Vec3Normalize_R( cr ) > 0.001f )
            {
                nrm[0] += cr[0]; nrm[1] += cr[1]; nrm[2] += cr[2];
                pos[0] += v11[0]; pos[1] += v11[1]; pos[2] += v11[2];
                ++count;
            }
        }
    }

    if ( Vec3Normalize_R( nrm ) < 0.001f )
        return 0;
    float dist = ( pos[2] * nrm[2] + pos[0] * nrm[0] + pos[1] * nrm[1] ) / (float)count;

    for ( int col = 0; col < w - 1; ++col )
        for ( int row = 0; row < cd->height - 1; ++row )
        {
            const float *v = V[col + row * w].xyz;
            float d = v[1] * nrm[1] + v[0] * nrm[0] + v[2] * nrm[2] - dist;
            if ( d > 1.0f || d < -1.0f )
                return 0;
        }

    if ( out )
    {
        out[0] = nrm[0]; out[1] = nrm[1]; out[2] = nrm[2]; out[3] = dist;
    }
    return 1;
}

// ── Patch_PlanarTexProject (0x4392e0) — project S/T onto a fitted plane ──────
static curvePatchDef_t *Patch_PlanarTexProject( patchMesh_t *p, int layer, float sampleSize,
                                                const float *planeNormal )
{
    qtexture_s *lm = MaterialDef_GetLayeredMaterial( (MaterialDef *)( &p->texture + layer ) );
    int width  = lm ? lm->width  : 512;
    int height = lm ? lm->height : 512;
    float params[6];
    params[0] = (float)width  * sampleSize;
    params[1] = sampleSize * (float)height;
    params[2] = params[3] = params[4] = params[5] = 0.0f;
    Patch_ST( params, planeNormal, p, layer );
    return p->curveDef;
}

// ── Patch_TerrainTexProject (0x439350) — project a terrain patch's S/T onto the
//    dominant axis (no adaptive subdivision — terrain is its own control grid) ────
static curvePatchDef_t *Patch_TerrainTexProject( patchMesh_t *p, int layer, float sampleSize )
{
    if ( !p->curveDef )
        p->curveDef = Patch_GenericMesh2( p, layer, 0, 0 );
    curvePatchDef_t *cd = p->curveDef;
    curveVert_t     *V  = cd->verts;

    float summed[3] = { 0, 0, 0 };
    for ( int y = 0; y < cd->height - 1; ++y )
    {
        for ( int i = 0; i < cd->width - 1; ++i )
        {
            int pt = i + y * cd->width;
            const float *base, *eA, *eB, *eC;
            if ( p->ctrl[i][y].turned_edge & 1 )
            {
                base = V[pt + cd->width + 1].xyz;  // (i+1,y+1)
                eA   = V[pt + cd->width].xyz;      // (i,  y+1)
                eB   = V[pt].xyz;                  // (i,  y)
                eC   = V[pt + 1].xyz;              // (i+1,y)
            }
            else
            {
                base = V[pt + 1].xyz;              // (i+1,y)
                eA   = V[pt + cd->width + 1].xyz;  // (i+1,y+1)
                eB   = V[pt + cd->width].xyz;      // (i,  y+1)
                eC   = V[pt].xyz;                  // (i,  y)
            }
            float a[3] = { eA[0] - base[0], eA[1] - base[1], eA[2] - base[2] };
            float b[3] = { eB[0] - base[0], eB[1] - base[1], eB[2] - base[2] };
            float c[3] = { eC[0] - base[0], eC[1] - base[1], eC[2] - base[2] };
            float cr[3];
            Vec3Cross( b, a, cr );
            summed[0] += cr[0]; summed[1] += cr[1]; summed[2] += cr[2];
            Vec3Cross( c, b, cr );
            summed[0] += cr[0]; summed[1] += cr[1]; summed[2] += cr[2];
        }
    }

    float planeNormal[3] = { 0, 0, 0 };
    planeNormal[ Vec3_MajorAxis( summed ) ] = 1.0f; // axis with the largest |component|

    qtexture_s *lm = MaterialDef_GetLayeredMaterial( (MaterialDef *)( &p->texture + layer ) );
    int width  = lm ? lm->width  : 512;
    int height = lm ? lm->height : 512;
    float params[6];
    params[0] = (float)width  * sampleSize;
    params[1] = sampleSize * (float)height;
    params[2] = params[3] = params[4] = params[5] = 0.0f;
    Patch_ST( params, planeNormal, p, layer );
    return p->curveDef;
}

// ── Patch_ResampleCurveST (0x439580) — the curve (non-terrain) sample-size resampler ───────
//  Build the tessellated mesh (with col/row index maps), adaptively re-subdivide it, and
//  EITHER project onto the fitted plane (planar) OR spread each control point's S/T to the
//  normalized position (refinedIndex+0.5)/512 of its mapped refined vertex.
static void Patch_ResampleCurveST( patchMesh_t *p, int layer, float sampleSize )
{
    // The layer is the caller's — NOT g_qeglobals.current_edit_layer.  Patch_SetSampleSize
    // passes 1 (the lightmap layer); the Patch_Rebuild/BrushToMesh refinement callers pass
    // their own layer.
    static int colCounts[512], rowCounts[512], colMap[512], rowMap[512];

    curvePatchDef_t *base = Patch_GenericMesh2( p, layer,
                                                colMap, rowMap );
    curvePatchDef_t *refined = Curve_AdaptiveSubdivide( base, sampleSize,
                                                        colCounts, rowCounts, colMap, rowMap );
    free( base );

    float plane[4];
    if ( Curve_FitPlane( refined, plane ) )
    {
        free( refined );
        Patch_PlanarTexProject( p, layer, sampleSize, plane );
        return;
    }

    const int rw = refined->width;
    const int rh = refined->height;
    for ( int col = 0; col < p->width; ++col )
    {
        int j = 0;
        if ( rw >= 2 )
            for ( ; j < rw - 1; ++j ) if ( colMap[j] >= col ) break;
        float s = (float)( ( (double)j + 0.5 ) * 0.001953125 );   // (j+0.5)/512
        iassert( isfinite( s ) );

        for ( int row = 0; row < p->height; ++row )
        {
            int k = 0;
            if ( rh >= 2 )
                for ( ; k < rh - 1; ++k ) if ( rowMap[k] >= row ) break;
            float t = (float)( ( (double)k + 0.5 ) * 0.001953125 );
            iassert( isfinite( t ) );

            float *st = &p->ctrl[col][row].texCoord.st[2 * layer];
            st[0] = s;
            st[1] = t;
        }
    }
    free( refined );
}

// ── Patch_SetSampleSize (0x442b00) — set a patch's texture SAMPLE SIZE ──────────────────
//  Reached from Brush_SetAllFacesSampleSize (0x477080) when a selected brush owns a patch —
//  the Surface Inspector "sample size" field with a patch selected.  Re-sample the patch
//  texCoords at the new density (terrain → dominant-axis projection; curves → adaptive
//  spread / planar projection), store the size + mark bDirty, and (layer 1) rebuild the
//  render curveDef.
void Patch_SetSampleSize( patchMesh_t *p, int sampleSize )
{
    const float sz = (float)sampleSize;

    // Layer 1 (the lightmap) is hardcoded for BOTH branches — the resample always targets
    // layer 1; only the curveDef REBUILD below is gated on current_edit_layer==1.
    if ( ( p->type & PATCH_TERRAIN ) != 0 )
        Patch_TerrainTexProject( p, 1, sz );
    else
        Patch_ResampleCurveST( p, 1, sz );

    p->bDirty = 1;
    *(float *)&p->size_of_struct_0x504C = sz;

    if ( g_qeglobals.current_edit_layer == 1 )
    {
        if ( p->curveDef )
            free( p->curveDef );
        p->curveDef = Patch_GenericMesh2( p, g_qeglobals.current_edit_layer, 0, 0 );
        ++p->version;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x44cde0  Patch_Subdivide  (PMESH.CPP:8996)
// Steps every selected non-terrain patch's tessellation level (subDivType) by
// `decrease` (negative coarsens, positive refines), rebuilding curveDef until its
// vertex count actually changes or it clamps at subDivType 1.  When refining, only
// acts on patches whose tessellated grid already exceeds the control grid.  Called
// from CMainFrame::OnOverBrightShift{Up,Down}.
// ─────────────────────────────────────────────────────────────────────────────
void Patch_Subdivide( int decrease )
{
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        patch_t *patch = b->patch;
        if ( !patch || ( patch->def->type & PATCH_TERRAIN ) != 0 )
            continue;

        iassert( b->patch->def == b->def->patch );

        patchMesh_t *def = b->patch->def;
        if ( decrease < 0 && def->subDivType == 1 )
            continue;

        if ( decrease <= 0
             || def->curveDef->width  > def->width
             || def->curveDef->height > def->height )
        {
            int origCount = def->curveDef->width * def->curveDef->height;
            def->subDivType += decrease;
            if ( def->subDivType < 1 )
            {
                def->subDivType = 1;
            }
            else
            {
                while ( 1 )
                {
                    if ( def->curveDef )
                        free( def->curveDef );
                    curvePatchDef_t *newDef = Patch_GenericMesh2( def, g_qeglobals.current_edit_layer, 0, 0 );
                    ++def->version;
                    def->curveDef = newDef;
                    if ( newDef->width * newDef->height != origCount )
                        break;
                    def->subDivType += decrease;
                    if ( def->subDivType < 1 )
                    {
                        def->subDivType = 1;
                        break;
                    }
                }
            }
        }
    }
    g_nUpdateBits = -1;
}

// ════════════════════════════════════════════════════════════════════════════
//  VERT-SNAP COLLECTOR SUBSYSTEM (the V-key mixed patch+brush vert-edit entry).
//  Reached via the V key (cmd 33005, CMainFrame::OnSelectionDragVertices third arm) with
//  a mixed patch+brush selection.  Clears the patch_verts_array01 vert pool, then walks the selected brush
//  list and, for each brush/patch/prefab/misc_model whose vert-snap PREF is enabled,
//  collects its world-space vertices into patch_verts_array01 (dedup'd within 0.1u).
//  That pool is the snap target the subsequent curve-point drag snaps to.  All three
//  vert-snap prefs (m_bVertSnapModel/Brush/Prefab) DEFAULT OFF.
//
//  STORAGE LAYOUT:
//    Each collected vert is a vec3 in a flat float array whose base is
//    &g_qeglobals.unkown_pmesh_float1: element N is at [3*N+0/1/2].  The binary's dedup
//    scan reads through g_qeglobals.patch_verts_array01 (2 floats past that base) with
//    offsets -8/-4/0, so the store and the read address the SAME vec3 array, 12-byte
//    stride (the two qeglobals members deliberately overlap).  Both views resolve to
//    VertSnapPool() below.
// ════════════════════════════════════════════════════════════════════════════

// The single flat vec3 vert pool both the collectors (store) and VertSnap_FindDup (read)
// address.  Base = &g_qeglobals.unkown_pmesh_float1.
static inline float *VertSnapPool() { return (float *)&g_qeglobals.unkown_pmesh_float1; }

// ── VertSnap_FindDup (0x43c240) — dedup scan ───────────────────────────────────────
//  Returns the index of the first pooled vert within sqrt(0.01)=0.1u of `cand`,
//  else -1.  Reads VertSnapPool()[3*i + 0/1/2] (see the layout note above).
//  count<=0 → -1 (the loop's pre-test).
static signed int VertSnap_FindDup( const float *cand )
{
    if ( g_qeglobals.patch_verts_array01_count <= 0 )
        return -1;

    const float *pool = VertSnapPool();
    int i = 0;
    for ( ;; i += 1 )
    {
        float dx = pool[3 * i + 0] - cand[0];
        float dy = pool[3 * i + 1] - cand[1];
        float dz = pool[3 * i + 2] - cand[2];
        if ( dx * dx + dy * dy + dz * dz < 0.01 )   // compare vs double 0.01
            return i;
        if ( i + 1 >= g_qeglobals.patch_verts_array01_count )
            return -1;
    }
}

// Store one (already world-space) vert into the pool if it isn't a duplicate; returns
// true once the pool fills (0x4000 = 16384 verts).  Common tail of all three collectors.
static inline bool VertSnap_Store( const float *p )
{
    if ( VertSnap_FindDup( p ) >= 0 )
        return false;
    float *pool = VertSnapPool();
    int n = g_qeglobals.patch_verts_array01_count;
    pool[3 * n + 0] = p[0];
    pool[3 * n + 1] = p[1];
    pool[3 * n + 2] = p[2];
    return ( ++g_qeglobals.patch_verts_array01_count == 0x4000 );
}

// ── VertSnap_CollectBrush (0x442520) — brush winding vert collector ──────────────────────
//  For each face of the brush DEF, world-transform (through `orient`) each winding
//  point and dedup-store it into the pool.
static void VertSnap_CollectBrush( const orientation_t *orient, selbrush_t *b )
{
    brush_t *def = b->def;
    if ( !def->faceCount )
        return;

    for ( int fi = 0; fi < def->faceCount; ++fi )
    {
        winding_t *w = def->faces[fi].w;
        if ( !w || w->numpoints == 0 )
            continue;

        const float *pts = (const float *)&w->p[0][0];     // point j = pts[3*j..3*j+2]
        for ( int j = 0; j < w->numpoints; ++j )
        {
            float world[3];
            OrientationPosToWorldPos( world, pts + 3 * j, orient );
            if ( VertSnap_Store( world ) )
                return;                                     // pool full
        }
    }
}

// ── VertSnap_CollectPatch (0x4422b0) — patch control-grid vert collector ───────────────────
//  Walk a patch's width×height control grid, world-transform each control point and
//  dedup-store it.  The binary INLINES OrientationPosToWorldPos (and its
//  q_shared.cpp:1599 `pos != out` assert); the real function carries that same assert.
//  Keeps the PMESH.CPP:4427 `b->patch->def == b->def->patch` instance/def-consistency
//  assert.
static void VertSnap_CollectPatch( selbrush_t *b, const orientation_t *orient )
{
    iassert( b->patch->def == b->def->patch );

    patchMesh_t *def = b->patch->def;
    for ( int row = 0; row < def->width; ++row )
    {
        for ( int col = 0; col < def->height; ++col )
        {
            float world[3];
            OrientationPosToWorldPos( world, def->ctrl[row][col].xyz, orient );
            if ( VertSnap_Store( world ) )
                return;                                     // pool full
        }
    }
}

// ── VertSnap_CollectModel (0x442410) — misc_model XModel vert collector ──────────────────
//  Extract the misc_model's LOD-0 geometry (Editor_ExtractXModelGeo, gfx_d3d/r_xsurface.cpp),
//  then world-transform each INDEXED vertex through `orient` and dedup-store it into the
//  snap pool.  XModel handle chain: b->owner->def->modelClass->model->handle.
struct XModel;
extern int __cdecl Editor_ExtractXModelGeo( XModel *model, float *verts, int vertLimit,
                                            uint16_t *indices, int indexLimit );  // gfx_d3d/r_xsurface.cpp (0x4FEBB0)
static void VertSnap_CollectModel( selbrush_t *b, const orientation_t *orient )
{
    entity_s_def  *def   = (entity_s_def *)b->owner->def;
    entitymodel_t *mc    = (entitymodel_t *)def->modelClass;
    modelnode_t   *model = mc->model;
    XModel        *xmodel = (XModel *)(intptr_t)model->handle;

    // 16384 vec3 positions + 65536 indices: a function-local static (editor is
    // single-threaded + non-reentrant, like DrawGeo's s_lineVerts) avoids a ~327 KB stack frame.
    static float    s_modelVerts[0x4000 * 3];   // 16384 positions
    static uint16_t s_modelIndices[0x10000];    // 65536 indices

    int indexCount = Editor_ExtractXModelGeo( xmodel, s_modelVerts, 0x4000,
                                              s_modelIndices, 0x10000 );
    for ( int i = 0; i < indexCount; ++i )
    {
        float world[3];
        OrientationPosToWorldPos( world, &s_modelVerts[3 * s_modelIndices[i]], orient );
        if ( VertSnap_Store( world ) )
            return;                              // pool full
    }
}

// ── VertSnapTo_ModelBrushPrefab (0x442160) — recursive snap walker ─────────────
//  Walk a brush instance list (top call: &selected_brushes; prefab recursion: the
//  prefab's active_brushlist).  Per brush, dispatch by its owning entity's eclass:
//    * fixedsize entity:
//        - prefab (classtype & 0x10) + m_bVertSnapPrefab → recurse into the prefab's
//          active_brushlist with the composed orientation;
//        - else if Model_SetModel && eclass=="misc_model" + m_bVertSnapModel →
//          collect the model's verts (VertSnap_CollectModel).
//    * non-fixedsize (brush/patch) + m_bVertSnapBrush:
//        - patch brush: collect its control grid (VertSnap_CollectPatch) — but ONLY
//          when this is NOT the top-level selected list (listHead != &selected_brushes),
//          i.e. only patches reached via a prefab recursion are snapped to (a selected
//          patch is the one being EDITED, not a snap target);
//        - solid brush: collect its winding verts (VertSnap_CollectBrush).
//  Stops once the pool reaches 0x4000.  Returns nothing (the binary's return value, a
//  scratch entity pointer, is never consumed by the caller).
static void VertSnapTo_ModelBrushPrefab( selbrush_t *listHead, orientation_t *orient )
{
    selbrush_t *next = listHead->next;
    if ( next == listHead )
        return;

    while ( g_qeglobals.patch_verts_array01_count < 0x4000 )
    {
        selbrush_t   *cur = next;
        entity_s_def *ownerDef = (entity_s_def *)cur->owner->def;
        eclass_t     *eclass   = ownerDef->eclass;

        if ( *(int *)&eclass->fixedsize )
        {
            if ( ( eclass->classtype & 0x10 ) != 0 )       // prefab entity
            {
                if ( g_PrefsDlg->m_bVertSnapPrefab )
                {
                    orientation_t composed;
                    Entity_GetOrientation( ownerDef, orient, &composed );
                    // &prefab->active_brushlist (the selbrush_t sentinel).
                    selbrush_t *prefabList =
                        &((prefab_s *)cur->owner->prefab)->brushes;
                    VertSnapTo_ModelBrushPrefab( prefabList, &composed );
                }
            }
            else                                           // model entity (misc_model etc.)
            {
                if ( Model_SetModel( cur, (const float *)orient ) )
                {
                    entity_s_def *ed = (entity_s_def *)cur->owner->def;
                    if ( !strcmp( ed->eclass->name, "misc_model" ) )
                    {
                        if ( g_PrefsDlg->m_bVertSnapModel )
                        {
                            orientation_t composed;
                            Entity_GetOrientation( ed, orient, &composed );
                            VertSnap_CollectModel( cur, &composed );
                        }
                    }
                }
            }
        }
        else if ( g_PrefsDlg->m_bVertSnapBrush )           // brush / patch geometry
        {
            if ( cur->patch )
            {
                if ( listHead != &selected_brushes )       // only snap to prefab patches
                    VertSnap_CollectPatch( cur, orient );
            }
            else
            {
                VertSnap_CollectBrush( orient, cur );
            }
        }

        next = cur->next;
        if ( next == listHead )
            break;
    }
}

// ── Terrain_Edit (0x442100) — MISNAMED (it is NOT a terrain editor) ─
//  The V-key vert-edit entry for a MIXED patch+brush selection (the third arm of
//  CMainFrame::OnSelectionDragVertices, cmd 33005).  Clears the point/move-point/vert
//  pools, runs the vert-snap collector over the selection (through the identity world
//  orientation), switches to curve-point edit mode, and runs the mode-transition tail
//  (sel_cycle_edge_direction_quad → refresh patch toolbar).
void Terrain_Edit()
{
    g_qeglobals.d_numpoints              = 0;
    g_qeglobals.d_num_move_points        = 0;
    g_qeglobals.patch_verts_array01_count = 0;
    g_qeglobals.patch_verts_array02_count = 0;

    VertSnapTo_ModelBrushPrefab( &selected_brushes,
                                 (orientation_t *)world_orient_matrix );

    select_t prevMode = g_qeglobals.d_select_mode;
    g_qeglobals.d_select_mode = sel_curvepoint;
    if ( prevMode == sel_cycle_edge_direction_quad )
        CMainFrame_UpdatePatchToolbarButtons();
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x441AD0  Patch_DrawSubmitFaceWindings — patch shadow-caster winding submit (the patch
//  analogue of Brush_DrawSubmitFaceWindings).  Realizes the patch material; if it
//  isn't a shadow-casting drawn material it returns.  Then walks the patch's
//  triangulated index list, building per-triangle (or per-coplanar-quad) world
//  windings and feeding them to the light-region CSG (Region_AddWindingFace).
//  The vertex array is def->curveDef->verts (stride 44, xyz at +0).
// ═════════════════════════════════════════════════════════════════════════════
#include "primarylights_region.h"
extern int    MaterialDef_06( qtexture_s *radMtl );                         // 0x4318A0
extern void   Com_PrintMessage( const char *fmt, ... );
extern bool   Material_CastsStencilShadow( Material *m );                   // 0x4FEE90

// Patch_Vec3Dot (0x40A4A0) — vec3 dot.
static float Patch_Vec3Dot( const float *a, const float *b )
{
    return (float)( a[1] * b[1] + a[0] * b[0] + a[2] * b[2] );
}

// Patch_EdgePlane (0x441930) — edge-clip plane: plane normal = cross(faceNormal, edgeA-edgeB),
// normalized; plane.dist = normal·edgeB.
static void Patch_EdgePlane( const float *edgeA, const float *faceNormal, const float *edgeB, float *out )
{
    float edge[3];
    edge[0] = edgeA[0] - edgeB[0];
    edge[1] = edgeA[1] - edgeB[1];
    edge[2] = edgeA[2] - edgeB[2];
    Vec3Cross( faceNormal, edge, out );
    Vec3Normalize_R( out );
    out[3] = out[1] * edgeB[1] + edgeB[0] * out[0] + out[2] * edgeB[2];
}

// Patch_QuadIsConvex (0x441990) — true if the two triangles {p0,p1,p2} and {p2,p3,p0} of a
// patch quad are coplanar AND convex enough to emit as a single quad.  `q` holds the six
// corner pointers from Patch_DrawSubmitFaceWindings.
struct PatchQuad { const float *p0, *p1, *p2, *p3, *p4, *p5; };
static char Patch_QuadIsConvex( const PatchQuad *q )
{
    // requires p2 == p3 and p1 == p4 (degenerate shared edges), then plane test of the
    // four corners against a reference.
    if ( q->p2 != q->p3 )
        return 0;
    if ( q->p1 != q->p4 )
        return 0;
    float plane[4];
    if ( !PlaneFromPoints_Real( plane, (float *)q->p1, (float *)q->p0, (float *)q->p2 ) )
        return 0;
    const float *ref = q->p5;
    float d = ref[1] * plane[1] + ref[0] * plane[0] + ref[2] * plane[2] - plane[3];
    if ( I_fabs( d ) >= 0.02500000037252903f )
        return 0;
    float e[4];
    Patch_EdgePlane( q->p2, plane, q->p1, e );
    float d2 = ref[1] * e[1] + ref[0] * e[0] + ref[2] * e[2];
    if ( d2 - e[3] < -0.02500000037252903f )
        return 0;
    Patch_EdgePlane( q->p0, plane, q->p2, e );
    if ( Patch_Vec3Dot( e, ref ) - e[3] > 0.02500000037252903f )
        return 0;
    Patch_EdgePlane( q->p1, plane, q->p0, e );
    if ( Patch_Vec3Dot( e, ref ) - e[3] > 0.02500000037252903f )
        return 0;
    return 1;
}

void Patch_DrawSubmitFaceWindings( patch_t *patchInst, const orientation_t *orient, float *desc, rface_t **outList )
{
    patch_t *pm = (patch_t *)(intptr_t)patchInst;
    MaterialDef *md = (MaterialDef *)&pm->def->texture;

    dword_181F51C = 0x2000;
    MaterialDef_02( md, MaterialDef_06 );
    int exterior;
    if ( dword_181F51C )
    {
        exterior = 0;   // a drawn material (the realize survivor cleared) → interior
    }
    else
    {
        // not drawn → must be a stencil-shadow layered material to submit at all
        // (MaterialDef_10_LayeredMatHandle carries the MaterialDef.cpp:246 assert)
        extern bool MaterialDef_10_LayeredMatHandle( MaterialDef *mtlDef );
        if ( !MaterialDef_10_LayeredMatHandle( md ) )
            return;
        exterior = 1;
    }

    iassert( pm->version == pm->def->version );   // PMESH.CPP:4236

    ++g_windingAlloc;
    winding_t *w = (winding_t *)malloc( 0x34u );
    if ( !w )
        Com_PrintMessage( "out of memory: winding_t\n" );
    w->numpoints = 0;

    int indexIter = 0;
    if ( pm->indexCount >= 6 )
    {
        for ( ; ; )
        {
            const ushort *indices = pm->indicesFront;
            const char *verts = (const char *)pm->def->curveDef->verts;       // GfxWorldVertex base
            #define VPTR(n) (const float *)( verts + 44 * indices[indexIter + (n)] )
            const float *p0 = VPTR(0), *p1 = VPTR(1), *p2 = VPTR(2);
            const float *p3 = VPTR(3), *p4 = VPTR(4), *p5 = VPTR(5);
            #undef VPTR
            PatchQuad q = { p0, p1, p2, p3, p4, p5 };
            if ( Patch_QuadIsConvex( &q ) )
            {
                // quad winding = {i0, i1, i5, i2} (i2==i3 here).
                w->numpoints = 4;
                OrientationPosToWorldPos( w->p[0], q.p0, orient );
                OrientationPosToWorldPos( w->p[1], q.p1, orient );
                OrientationPosToWorldPos( w->p[2], q.p5, orient );
                OrientationPosToWorldPos( w->p[3], q.p2, orient );
                Region_AddWindingFace( w, outList, desc, exterior );
            }
            else
            {
                w->numpoints = 3;
                OrientationPosToWorldPos( w->p[0], q.p0, orient );
                OrientationPosToWorldPos( w->p[1], q.p1, orient );
                OrientationPosToWorldPos( w->p[2], q.p2, orient );
                Region_AddWindingFace( w, outList, desc, exterior );
                OrientationPosToWorldPos( w->p[0], q.p3, orient );
                OrientationPosToWorldPos( w->p[1], q.p4, orient );
                OrientationPosToWorldPos( w->p[2], q.p5, orient );
                Region_AddWindingFace( w, outList, desc, exterior );
            }
            indexIter += 6;
            if ( indexIter + 6 > pm->indexCount )
                break;
        }
    }

    // tail triangle (indexCount not a multiple of 6)
    if ( indexIter != pm->indexCount )
    {
        vassert( indexIter + 3 == pm->indexCount, "%i, %i", indexIter + 3, pm->indexCount );   // PMESH.CPP:4274
        const ushort *indices = pm->indicesFront;
        const char *verts = (const char *)pm->def->curveDef->verts;
        const float *t0 = (const float *)( verts + 44 * indices[indexIter] );
        const float *t1 = (const float *)( verts + 44 * indices[indexIter + 1] );
        const float *t2 = (const float *)( verts + 44 * indices[indexIter + 2] );
        w->numpoints = 3;
        OrientationPosToWorldPos( w->p[0], t0, orient );
        OrientationPosToWorldPos( w->p[1], t1, orient );
        OrientationPosToWorldPos( w->p[2], t2, orient );
        Region_AddWindingFace( w, outList, desc, exterior );
    }

    --g_windingAlloc;
    free( w );
}

// ═════════════════════════════════════════════════════════════════════════════
//  sel_area 3D rect-select of patch/terrain CONTROL POINTS.  When the operator alt-drags a
//  marquee in the 3D camera view while a patch point-edit mode is active (sel_area),
//  Drag_MouseUp builds a 4-plane view frustum from the drag rect
//  (Camera_GetRectSelection3D, camwnd.cpp 0x403c10) and hands it to these two collectors,
//  which add every control point inside the frustum to the move-point sets.
//
//  Frustum plane layout (32-byte stride, written by Camera_GetRectSelection3D):
//    normal.x @0, normal.y @4, normal.z @8, dist (DOUBLE) @16, type(int) = -1 @24.
//  A point p is INSIDE plane i iff dot(normal_i, p) < dist_i (the binary breaks out of the
//  4-plane scan on the first `>= dist`; only points inside all 4 reach count == 4).
// ═════════════════════════════════════════════════════════════════════════════

struct edFrustumPlane_t          // the 32-byte plane Camera_GetRectSelection3D emits
{
    float  normal[3];            // @0
    int    pad_C;                // @12
    double dist;                 // @16
    int    type;                 // @24 (= -1)
    int    pad_1C;               // @28
};
static_assert( sizeof(edFrustumPlane_t) == 32, "edFrustumPlane_t" );

extern int Patch_FindSelectedArrayPoint( const float *pt );   // brush.cpp (0x43c1c0)

// Is `xyz` inside all 4 frustum planes?
//   The dot is rounded to a float32 ONCE, then compared against the DOUBLE plane dist:
//   accumulate in double, snap to float, then compare against the double dist.  (A
//   per-operation float accumulation would round 3× and could flip a 1-ULP boundary case.)
static bool Ed_PointInFrustum4( const edFrustumPlane_t *planes, const float *xyz )
{
    int n = 0;
    for ( ; n < 4; ++n )
    {
        float dot = (float)( (double)planes[n].normal[0] * xyz[0]
                           + (double)planes[n].normal[1] * xyz[1]
                           + (double)planes[n].normal[2] * xyz[2] );
        if ( (double)dot >= planes[n].dist )
            break;
    }
    return n == 4;
}

// ── Terrain_SelectAreaPointsPass (0x4481c0) ───────────────────────────────────
//  One pass over every selected PATCH's 16×16 control grid; for each control point
//  inside the frustum, either toggle the lock flag (turned_edge bit1, when the
//  lock/unlock prefs are active), or add/remove it from d_move_points[].  `removePass`
//  is the "second pass" flag (0 = add to a fresh set / remove-on-deselect; the caller
//  Terrain_SelectAreaPoints runs pass 0 first and pass 1 only if pass 0 added none).
//  Returns 1 iff this pass touched a point.
static int Terrain_SelectAreaPointsPass( const edFrustumPlane_t *planes, char removePass )
{
    byte touched = 0;

    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        patchMesh_t *patch = b->patch ? b->patch->def : nullptr;
        if ( !patch )
            continue;

        iassert( b->patch->def == b->def->patch );

        // grid: outer = ctrl[col][..] (width), inner = ctrl[col][row] (height).
        for ( int col = 0; col < patch->width; ++col )
        {
            for ( int row = 0; row < patch->height; ++row )
            {
                drawVert_t *cp = &patch->ctrl[col][row];
                if ( !Ed_PointInFrustum4( planes, cp->xyz ) )
                    continue;

                if ( removePass )
                {
                    // pass 1: REMOVE this control point from d_move_points[] (by ptr).
                    iassert( !g_qeglobals.bLockPatchVerts && !g_qeglobals.bUnlockPatchVerts );
                    int idx = 0;
                    int n = g_qeglobals.d_num_move_points;
                    for ( ; idx < n; ++idx )
                        if ( g_qeglobals.d_move_points[idx] == cp )
                            break;
                    if ( idx < n )
                    {
                        int rest = n - idx - 1;
                        if ( rest > 0 )
                            memmove( &g_qeglobals.d_move_points[idx],
                                     &g_qeglobals.d_move_points[idx + 1],
                                     sizeof( g_qeglobals.d_move_points[0] ) * rest );
                        g_qeglobals.d_num_move_points = n - 1;
                    }
                }
                else if ( g_qeglobals.bLockPatchVerts )
                {
                    cp->turned_edge |= 2;       // lock
                    touched = 1;
                }
                else if ( g_qeglobals.bUnlockPatchVerts )
                {
                    cp->turned_edge &= ~2;      // unlock
                    touched = 1;
                }
                else if ( ( cp->turned_edge & 2 ) == 0 )    // skip locked points
                {
                    // pass 0 ADD: queue this control point if not already present
                    // (by ptr).  Same drill-down head-insert as Patch_AppendMovePoint:
                    // if the owning patch is the first selected brush AND >1 brushes
                    // selected, insert at the front; else append.
                    int idx = 0;
                    int n = g_qeglobals.d_num_move_points;
                    for ( ; idx < n; ++idx )
                        if ( g_qeglobals.d_move_points[idx] == cp )
                            break;
                    if ( idx >= n )             // not already queued
                    {
                        touched = 1;
                        if ( n
                             && (brush_t *)patch->pSymbiot == selected_brushes.next->def
                             && selected_brushes.next != &selected_brushes )
                        {
                            memmove( &g_qeglobals.d_move_points[1],
                                     &g_qeglobals.d_move_points[0],
                                     sizeof( g_qeglobals.d_move_points[0] ) * n );
                            g_qeglobals.d_move_points[0] = cp;
                        }
                        else
                        {
                            g_qeglobals.d_move_points[n] = cp;
                        }
                        g_qeglobals.d_num_move_points = n + 1;
                    }
                }
            }
        }
    }

    return touched;
}

// ── Terrain_SelectAreaPoints (0x448460) ───────────────────────────────────────
//  Pass-0 first; if the lock/unlock prefs are off and pass 0 added nothing, retry
//  with pass 1 (the deselect/remove pass).  `select` (the caller's (buttons>>3)&1)
//  forces the fresh-set reset when off.
void Terrain_SelectAreaPoints( const void *planes, char select )
{
    int doSecondPass = 1;
    if ( !select || g_qeglobals.bLockPatchVerts || g_qeglobals.bUnlockPatchVerts )
    {
        g_qeglobals.d_num_move_points = 0;
        doSecondPass = 0;
    }
    g_nPatchClickedView = -1;
    int r = Terrain_SelectAreaPointsPass( (const edFrustumPlane_t *)planes, 0 );
    if ( !r && doSecondPass )
        Terrain_SelectAreaPointsPass( (const edFrustumPlane_t *)planes, 1 );
}

// ── Patch_SelectAreaPointsPass (0x4484c0) ─────────────────────────────────────
//  Pass over the DISPLAYED candidate-point list (patch_verts_array01: vec3's at
//  &unkown_pmesh_float1, count patch_verts_array01_count) — the curve-edit feedback
//  set built during the patch curve drag.  Each point inside the frustum is added to
//  (pass 0) or removed from (pass 1) the SELECTED set patch_verts_array02 (vec3's at
//  &unkown_pmesh_float2, count patch_verts_array02_count).  Returns 1 iff pass 0 added.
static int Patch_SelectAreaPointsPass( const edFrustumPlane_t *planes, char removePass )
{
    byte added = 0;

    if ( g_qeglobals.bLockPatchVerts || g_qeglobals.bUnlockPatchVerts )
        return 0;

    if ( g_qeglobals.patch_verts_array01_count <= 0 )
        return 0;

    float *pt   = &g_qeglobals.unkown_pmesh_float1;   // array01 base, vec3 stride
    float *sel0 = &g_qeglobals.unkown_pmesh_float2;   // array02 base, vec3 stride

    for ( int i = 0; i < g_qeglobals.patch_verts_array01_count; ++i, pt += 3 )
    {
        if ( !Ed_PointInFrustum4( planes, pt ) )
            continue;

        if ( removePass )
        {
            iassert( !g_qeglobals.bLockPatchVerts && !g_qeglobals.bUnlockPatchVerts );
            int idx = Patch_FindSelectedArrayPoint( pt );
            if ( idx >= 0 )
            {
                int rest = g_qeglobals.patch_verts_array02_count - idx - 1;
                if ( rest > 0 )
                    memmove( sel0 + 3 * idx, sel0 + 3 * ( idx + 1 ),
                             sizeof( float ) * 3 * rest );
                --g_qeglobals.patch_verts_array02_count;
            }
        }
        else if ( Patch_FindSelectedArrayPoint( pt ) < 0 )
        {
            float *dst = sel0 + 3 * g_qeglobals.patch_verts_array02_count;
            dst[0] = pt[0];
            dst[1] = pt[1];
            dst[2] = pt[2];
            ++g_qeglobals.patch_verts_array02_count;
            added = 1;
        }
    }

    return added;
}

// ── Patch_SelectAreaPoints (0x448620) ─────────────────────────────────────────
void Patch_SelectAreaPoints( const void *planes, char select )
{
    int doSecondPass = 1;
    if ( !select )
    {
        g_qeglobals.patch_verts_array02_count = 0;
        doSecondPass = 0;
    }
    g_nPatchClickedView = -1;
    int r = Patch_SelectAreaPointsPass( (const edFrustumPlane_t *)planes, 0 );
    if ( !r && doSecondPass )
        Patch_SelectAreaPointsPass( (const edFrustumPlane_t *)planes, 1 );
}

// ── Splay_Vec2Normalize (editor Vec2Normalize 0x4A4750) — normalize a 2D vector in
// place, return its length.  A length of 0 (or non-positive) leaves the vector
// untouched (divides by 1).  (Named to avoid clashing with the engine's com_math
// Vec2Normalize(vec2r).)
static double Splay_Vec2Normalize( float *v )
{
    double len = sqrt( (double)( v[0] * v[0] + v[1] * v[1] ) );
    float d = (float)len;
    if ( d <= 0.0f )
        d = 1.0f;
    float inv = 1.0f / d;
    v[0] = v[0] * inv;
    v[1] = v[1] * inv;
    return len;
}

// ── DoSplay (0x44A430) — Selection→Splay (33157, Ctrl+W) ──────────────────────────
// "Splays" the selected patch control points radially: computes their 2D (view-plane
// XY) bbox centre, then pushes each point to a fixed radius (Prefs→SplayDistance) along
// its direction from the centre.  Requires ONLY patches selected (no faces, all selected
// brushes are patches) and 2..1025 move-points picked.  Rebuilds every selected patch.
// (The binary's scratch copy of the pre-splay xyz is never read back; it is omitted.)

void DoSplay()
{
    if ( g_SelectedFaces.GetSize() > 0 )
    {
        Sys_Printf( "can only connect patch vertices if only patches are selected\n" );
        return;
    }
    if ( selected_brushes.next == &selected_brushes )
    {
        Sys_Printf( "can only connect patch vertices if only patches are selected\n" );
        return;
    }
    // Every selected brush must be a patch.
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
        if ( !b->patch )
        {
            Sys_Printf( "can only connect patch vertices if only patches are selected\n" );
            return;
        }

    if ( g_qeglobals.d_num_move_points < 2 )
    {
        Sys_Printf( "can only splay patch vertices if at least 2 vertices are selected\n" );
        return;
    }
    if ( (unsigned)g_qeglobals.d_num_move_points > 1025u )
    {
        Sys_Printf( "can only splay patch vertices if at most %i vertices are selected\n", 1025 );
        return;
    }

    // 2D (X/Y) bounding box of the picked control points.
    float minx = g_qeglobals.d_move_points[0]->xyz[0];
    float maxx = g_qeglobals.d_move_points[0]->xyz[0];
    float miny = g_qeglobals.d_move_points[0]->xyz[1];
    float maxy = g_qeglobals.d_move_points[0]->xyz[1];
    for ( int i = g_qeglobals.d_num_move_points - 1; i > 0; --i )
    {
        drawVert_t *v = g_qeglobals.d_move_points[i];
        if ( minx > (double)v->xyz[0] ) minx = v->xyz[0];
        if ( maxx < (double)v->xyz[0] ) maxx = v->xyz[0];
        if ( miny > (double)v->xyz[1] ) miny = v->xyz[1];
        if ( maxy < (double)v->xyz[1] ) maxy = v->xyz[1];
    }

    float cx = ( maxx + minx ) * 0.5f;
    float cy = ( maxy + miny ) * 0.5f;
    float radius = (float)g_PrefsDlg->splay;

    for ( int i = 0; i < g_qeglobals.d_num_move_points; ++i )
    {
        drawVert_t *v = g_qeglobals.d_move_points[i];
        float dir[2];
        dir[0] = v->xyz[0] - cx;
        dir[1] = v->xyz[1] - cy;
        Splay_Vec2Normalize( dir );
        v->xyz[0] = dir[0] * radius + cx;
        v->xyz[1] = radius * dir[1] + cy;
    }

    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
        if ( b->patch && b->patch->def )
            Patch_Rebuild( b->patch->def, 1 );
    g_nUpdateBits = -1;
}

// ═════════════════════════════════════════════════════════════════════════════
//  PATCH/CURVE OPERATION CLUSTER (Cap / Thicken / Weld / Split).
//  ctrl grid convention: patchMesh_t.ctrl[16][16] is p->ctrl[col][row].
// ═════════════════════════════════════════════════════════════════════════════

// externs the cluster needs (all ported elsewhere; declared here for locality)
#define selected_brushes_next ( selected_brushes.next )   // IDB 0x23F1868 == selected_brushes.next
extern void         Undo_SetIdForEntity( entity_s_def *ent );                // undo.cpp 0x45E9E0
extern void        *zero;                                                   // empty-string sentinel (engine_stubs.cpp)

// VectorCompare (0x44D810) — vec3 approx-equality (epsilon^2 = 1e-6).  Small helper,
// inlined here (not otherwise ported).
static int Vec3NearEqual( const float *a, const float *b )
{
    for ( int k = 0; k < 3; ++k )
    {
        float d = a[k] - b[k];
        if ( d * d > 0.00000100000011116208f )
            return 0;
    }
    return 1;
}

// 0x483B40 — is `classname` a weld-mergeable container (worldspawn /
// func_group / func_cullgroup)?  Inlined here (its only radiant caller is WeldMesh).
static char Classname_IsWeldContainer( const char *classname )
{
    static const char *names[3] = { "worldspawn", "func_group", "func_cullgroup" };
    for ( int i = 0; i < 3; ++i )
        if ( !strcmp( classname, names[i] ) )
            return 1;
    return 0;
}

// Region_BoundsMidpoint (0x409F20) — midpoint of two vec3 (static in brush.cpp;
// re-inlined here for the CapSpecial endcap).
static void Vec3Midpoint( const float *a, const float *b, float *out )
{
    out[0] = ( a[0] + b[0] ) * 0.5f;
    out[1] = ( a[1] + b[1] ) * 0.5f;
    out[2] = ( a[2] + b[2] ) * 0.5f;
}

// The Curve→Cap "special cap" dialog (CCapDialog 0x40A8A0, IDD 0xA1) — headless: the
// port never pops it; Patch_CapCurrent handles the no-edge-matched case as a no-op.

// ── Patch_CapFillGrid (0x439BA0) — replicate one row's xyz down every ─
//   row of the cap grid.  p = the just-created cap patchMesh_t, xyz = a source vec3
//   (the picked edge point).  For every control point (col, row) of the cap: xyz.
static void Patch_CapFillGrid( const float *xyz, patchMesh_t *p )
{
    for ( int col = 0; col < p->width; ++col )
        for ( int row = 0; row < p->height; ++row )
        {
            p->ctrl[col][row].xyz[0] = xyz[0];
            p->ctrl[col][row].xyz[1] = xyz[1];
            p->ctrl[col][row].xyz[2] = xyz[2];
        }
}

// cap perimeter / interior index tables.  Each entry is a {col,row} pair.  The 3x3 form is used for a source edge of length <= 9, the 5x5
// form for length > 9 (a longer edge needs a finer cap).
static const int s_capPerimeter3x3[8][2] =
{ {0,0},{1,0},{2,0},{2,1},{2,2},{1,2},{0,2},{0,1} };
static const int s_capPerimeter5x5[16][2] =
{ {0,0},{1,0},{2,0},{3,0},{4,0},{4,1},{4,2},{4,3},
  {4,4},{3,4},{2,4},{1,4},{0,4},{0,3},{0,2},{0,1} };
static const int s_capInterior3x3[1][2] = { {1,1} };
static const int s_capInterior5x5[9][2] =
{ {1,1},{2,1},{3,1},{1,2},{2,2},{3,2},{1,3},{2,3},{3,3} };

// ── Patch_Cap (0x439C00) — build ONE cap patch off a source edge ──────────────
//   bByColumn selects which axis of the source patch `pm` supplies the edge; bFirst
//   selects the low (0) or high (width-1/height-1) end.  A 3x3 (edge<=9) or 5x5
//   (edge>9) generic mesh is created, its perimeter ring filled from the source edge
//   control points (via the tables), its interior filled with the perimeter's bbox
//   centre, then cap-textured (Patch_Rebuild + Patch_ST projection — identical tail
//   to Patch_CapTexture).  Returns the new symbiont brush instance (or 0).
static selbrush_t *Patch_Cap( patchMesh_t *pm, char bByColumn, char bFirst )
{
    // srcCount = number of edge control points; edgeIdx = the fixed index of the edge (a row
    // when walking the columns, a column when walking the rows).
    // KIWI: the cap size follows the edge's own length - the binary picked it from the width
    // whatever the edge (> 9 -> 5x5) and walked srcCount entries of the 8-entry 3x3 ring
    // table (a closed 9-wide edge read one past it; a 10+ edge along the rows read further
    // and wrote stray cells).  And edgeIdx's last-edge index comes from the OTHER axis, as in
    // the binary (the port had them swapped, so a far-edge cap read the wrong row/column).
    const int srcCount = bByColumn ? pm->width : pm->height;
    const int edgeIdx  = bFirst ? 0 : ( bByColumn ? pm->height : pm->width ) - 1;
    const char bIs3x3  = ( srcCount <= 9 );   // 1 = 3x3 cap, 0 = 5x5 cap
    selbrush_t *b = bIs3x3 ? Patch_GenericMesh( 3, 3, 2, 0, 0 ) : Patch_GenericMesh( 5, 5, 2, 0, 0 );
    if ( !b )
    {
        Sys_Printf( "Unable to cap. You may need to ungroup the patch.\n" );
        return 0;
    }
    iassert( b->patch->def == b->def->patch );
    patchMesh_t *def = b->patch->def;
    def->type = (PATCH_TYPES)( def->type | PATCH_CAP );

    // bbox accumulators for the interior fill (mins/maxs, VectorMaxValues convention).
    float mins[3] = { 131072.0f, 131072.0f, 131072.0f };
    float maxs[3] = { -131072.0f, -131072.0f, -131072.0f };

    // seed the whole cap grid with the edge's first point, then fill the ring (the binary
    // seeded with ctrl[0][edgeIdx] for both walks - off the edge when walking the rows; KIWI
    // seeds with the edge's own first point, which ring slots past srcCount then repeat).
    const drawVert_t *edgeBase = bByColumn ? &pm->ctrl[0][edgeIdx] : &pm->ctrl[edgeIdx][0];
    Patch_CapFillGrid( edgeBase->xyz, def );

    // ── perimeter ring: map each source-edge control point onto the cap ring ──
    // (bounded by the ring: a closed edge's last point repeats its first)
    const int ringSize = bIs3x3 ? 8 : 16;
    for ( int i = 0; i < srcCount && i < ringSize; ++i )
    {
        // src point: bByColumn → walk down the columns (ctrl[i][edgeIdx]);
        //            else       → walk along the row (ctrl[edgeIdx][i]).
        const float *src = bByColumn
                         ? pm->ctrl[i][edgeIdx].xyz
                         : pm->ctrl[edgeIdx][i].xyz;

        // ring slot for this i (3x3 → perimeter of 3x3, 5x5 → perimeter of 5x5).
        int rc, rr;
        if ( bIs3x3 ) { rc = s_capPerimeter3x3[i][0]; rr = s_capPerimeter3x3[i][1]; }
        else          { rc = s_capPerimeter5x5[i][0]; rr = s_capPerimeter5x5[i][1]; }
        def->ctrl[rc][rr].xyz[0] = src[0];
        def->ctrl[rc][rr].xyz[1] = src[1];
        def->ctrl[rc][rr].xyz[2] = src[2];

        // VectorMaxValues(src, mins, maxs): mins = min(mins,src); maxs = max(maxs,src)
        // (0x4A8240; a FATAL stub in this build → inlined here).
        for ( int k = 0; k < 3; ++k )
        {
            if ( mins[k] > (double)src[k] ) mins[k] = src[k];
            if ( maxs[k] < (double)src[k] ) maxs[k] = src[k];
        }
    }

    // ── interior fill: centre = (mins+maxs)*0.5, written to every interior slot ──
    float centre[3];
    centre[0] = ( mins[0] + maxs[0] ) * 0.5f;
    centre[1] = ( mins[1] + maxs[1] ) * 0.5f;
    centre[2] = ( mins[2] + maxs[2] ) * 0.5f;
    int interiorCount = bIs3x3 ? 1 : 9;
    for ( int i = 0; i < interiorCount; ++i )
    {
        int ic, ir;
        if ( bIs3x3 ) { ic = s_capInterior3x3[i][0]; ir = s_capInterior3x3[i][1]; }
        else          { ic = s_capInterior5x5[i][0]; ir = s_capInterior5x5[i][1]; }
        def->ctrl[ic][ir].xyz[0] = centre[0];
        def->ctrl[ic][ir].xyz[1] = centre[1];
        def->ctrl[ic][ir].xyz[2] = centre[2];
    }

    // ── if bFirst, vertical-flip the cap grid (mirror each column's rows) so the ──
    //   winding faces outward (identical to patchInvert2's swap).
    if ( bFirst )
    {
        for ( int col = 0; col < def->width; ++col )
            for ( int r = 0; r < def->height / 2; ++r )
            {
                drawVert_t tmp                    = def->ctrl[col][r];
                def->ctrl[col][r]                 = def->ctrl[col][def->height - 1 - r];
                def->ctrl[col][def->height - 1 - r] = tmp;
            }
    }

    // ── cap texturing tail (identical to Patch_CapTexture( def, cap=0, s, s )) ──
    Patch_Rebuild( def, 1 );
    {
        const int layer = g_qeglobals.current_edit_layer;
        float s = g_qeglobals.random_texture_stuff[layer].sampleSize;
        Patch_CapTexture( def, 0, s, s );
    }
    iassert( def->pSymbiot == (entity_brush_s *)b->def );
    iassert( b->patch->def == def );
    iassert( b->def->patch == def );
    return b;
}

// ── Patch_CapSpecial (0x43A170) — dialog-driven "special" cap shape ───────────
//   nType selects the cap silhouette (bevel / endcap / inverted etc.); bFirst picks
//   the source row (0 or height-1).  Builds a 3x3 (or 5x3 for nType==1) cap grid by
//   sampling the source ROW's control points (ctrl[col][edgeIdx]) into fixed cap-grid
//   slots per shape, optionally vertical-flips, then cap-textures.
static selbrush_t *Patch_CapSpecial( patchMesh_t *pm, int nType, char bFirst )
{
    selbrush_t *b = ( nType == 3 ) ? Patch_GenericMesh( 5, 3, 2, 0, 0 )
                                   : Patch_GenericMesh( 3, 3, 2, 0, 0 );
    if ( !b )
    {
        Sys_Printf( "Unable to cap. Make sure you ungroup before re - capping." );
        return 0;
    }
    iassert( b->patch->def == b->def->patch );
    patchMesh_t *def = b->patch->def;
    def->type = (PATCH_TYPES)( def->type | PATCH_CAP );

    int edgeIdx = bFirst ? 0 : ( pm->height - 1 );

    // src[col] = &pm->ctrl[col][edgeIdx].xyz
    #define SRC(col) ( pm->ctrl[(col)][edgeIdx].xyz )

    float capMins[3], capMaxs[3];
    Patch_CalcBounds( capMins, capMaxs, pm, false );
    float mid[3];

    char bFlipMatch;   // whether this shape's grid needs the vertical flip
    if ( nType == 2 )
    {
        def->ctrl[0][0].xyz[0] = SRC(0)[0]; def->ctrl[0][0].xyz[1] = SRC(0)[1]; def->ctrl[0][0].xyz[2] = SRC(0)[2];
        def->ctrl[0][2].xyz[0] = SRC(2)[0]; def->ctrl[0][2].xyz[1] = SRC(2)[1]; def->ctrl[0][2].xyz[2] = SRC(2)[2];
        def->ctrl[0][1].xyz[0] = SRC(1)[0]; def->ctrl[0][1].xyz[1] = SRC(1)[1]; def->ctrl[0][1].xyz[2] = SRC(1)[2];
        def->ctrl[2][2].xyz[0] = SRC(1)[0]; def->ctrl[2][2].xyz[1] = SRC(1)[1]; def->ctrl[2][2].xyz[2] = SRC(1)[2];
        def->ctrl[1][0].xyz[0] = SRC(1)[0]; def->ctrl[1][0].xyz[1] = SRC(1)[1]; def->ctrl[1][0].xyz[2] = SRC(1)[2];
        def->ctrl[1][1].xyz[0] = SRC(1)[0]; def->ctrl[1][1].xyz[1] = SRC(1)[1]; def->ctrl[1][1].xyz[2] = SRC(1)[2];
        def->ctrl[1][2].xyz[0] = SRC(1)[0]; def->ctrl[1][2].xyz[1] = SRC(1)[1]; def->ctrl[1][2].xyz[2] = SRC(1)[2];
        def->ctrl[2][0].xyz[0] = SRC(1)[0]; def->ctrl[2][0].xyz[1] = SRC(1)[1]; def->ctrl[2][0].xyz[2] = SRC(1)[2];
        def->ctrl[2][1].xyz[0] = SRC(1)[0]; def->ctrl[2][1].xyz[1] = SRC(1)[1]; def->ctrl[2][1].xyz[2] = SRC(1)[2];
        bFlipMatch = 0;
    }
    else if ( nType == 0 )
    {
        // bevel: ctrl[2][2] = SRC(2) + (SRC(0) - SRC(1)); centre column/rows = that pt.
        def->ctrl[0][0].xyz[0] = SRC(2)[0]; def->ctrl[0][0].xyz[1] = SRC(2)[1]; def->ctrl[0][0].xyz[2] = SRC(2)[2];
        def->ctrl[0][1].xyz[0] = SRC(1)[0]; def->ctrl[0][1].xyz[1] = SRC(1)[1]; def->ctrl[0][1].xyz[2] = SRC(1)[2];
        def->ctrl[0][2].xyz[0] = SRC(0)[0]; def->ctrl[0][2].xyz[1] = SRC(0)[1]; def->ctrl[0][2].xyz[2] = SRC(0)[2];
        mid[0] = SRC(2)[0] + ( SRC(0)[0] - SRC(1)[0] );
        mid[1] = SRC(2)[1] + ( SRC(0)[1] - SRC(1)[1] );
        mid[2] = SRC(2)[2] + ( SRC(0)[2] - SRC(1)[2] );
        def->ctrl[2][2].xyz[0] = mid[0]; def->ctrl[2][2].xyz[1] = mid[1]; def->ctrl[2][2].xyz[2] = mid[2];
        def->ctrl[1][0].xyz[0] = mid[0]; def->ctrl[1][0].xyz[1] = mid[1]; def->ctrl[1][0].xyz[2] = mid[2];
        def->ctrl[1][1].xyz[0] = mid[0]; def->ctrl[1][1].xyz[1] = mid[1]; def->ctrl[1][1].xyz[2] = mid[2];
        def->ctrl[1][2].xyz[0] = mid[0]; def->ctrl[1][2].xyz[1] = mid[1]; def->ctrl[1][2].xyz[2] = mid[2];
        def->ctrl[2][0].xyz[0] = mid[0]; def->ctrl[2][0].xyz[1] = mid[1]; def->ctrl[2][0].xyz[2] = mid[2];
        def->ctrl[2][1].xyz[0] = mid[0]; def->ctrl[2][1].xyz[1] = mid[1]; def->ctrl[2][1].xyz[2] = mid[2];
        bFlipMatch = 1;
    }
    else if ( nType == 1 )
    {
        // endcap: interior column midpoint via Region_BoundsMidpoint(SRC(4), SRC(0)).
        Vec3Midpoint( SRC(4), SRC(0), mid );
        def->ctrl[0][0].xyz[0] = SRC(0)[0]; def->ctrl[0][0].xyz[1] = SRC(0)[1]; def->ctrl[0][0].xyz[2] = SRC(0)[2];
        def->ctrl[1][0].xyz[0] = mid[0];    def->ctrl[1][0].xyz[1] = mid[1];    def->ctrl[1][0].xyz[2] = mid[2];
        def->ctrl[2][0].xyz[0] = SRC(4)[0]; def->ctrl[2][0].xyz[1] = SRC(4)[1]; def->ctrl[2][0].xyz[2] = SRC(4)[2];
        def->ctrl[0][2].xyz[0] = SRC(2)[0]; def->ctrl[0][2].xyz[1] = SRC(2)[1]; def->ctrl[0][2].xyz[2] = SRC(2)[2];
        def->ctrl[1][2].xyz[0] = SRC(2)[0]; def->ctrl[1][2].xyz[1] = SRC(2)[1]; def->ctrl[1][2].xyz[2] = SRC(2)[2];
        def->ctrl[2][2].xyz[0] = SRC(2)[0]; def->ctrl[2][2].xyz[1] = SRC(2)[1]; def->ctrl[2][2].xyz[2] = SRC(2)[2];
        def->ctrl[1][1].xyz[0] = SRC(2)[0]; def->ctrl[1][1].xyz[1] = SRC(2)[1]; def->ctrl[1][1].xyz[2] = SRC(2)[2];
        def->ctrl[0][1].xyz[0] = SRC(1)[0]; def->ctrl[0][1].xyz[1] = SRC(1)[1]; def->ctrl[0][1].xyz[2] = SRC(1)[2];
        def->ctrl[2][1].xyz[0] = SRC(3)[0]; def->ctrl[2][1].xyz[1] = SRC(3)[1]; def->ctrl[2][1].xyz[2] = SRC(3)[2];
        bFlipMatch = 0;
    }
    else   // nType == 3 (5x3 endcap, sampling all 5 source columns)
    {
        def->ctrl[0][0].xyz[0] = SRC(0)[0]; def->ctrl[0][0].xyz[1] = SRC(0)[1]; def->ctrl[0][0].xyz[2] = SRC(0)[2];
        def->ctrl[1][0].xyz[0] = SRC(1)[0]; def->ctrl[1][0].xyz[1] = SRC(1)[1]; def->ctrl[1][0].xyz[2] = SRC(1)[2];
        def->ctrl[2][0].xyz[0] = SRC(2)[0]; def->ctrl[2][0].xyz[1] = SRC(2)[1]; def->ctrl[2][0].xyz[2] = SRC(2)[2];
        def->ctrl[3][0].xyz[0] = SRC(3)[0]; def->ctrl[3][0].xyz[1] = SRC(3)[1]; def->ctrl[3][0].xyz[2] = SRC(3)[2];
        def->ctrl[4][0].xyz[0] = SRC(4)[0]; def->ctrl[4][0].xyz[1] = SRC(4)[1]; def->ctrl[4][0].xyz[2] = SRC(4)[2];
        def->ctrl[0][1].xyz[0] = SRC(1)[0]; def->ctrl[0][1].xyz[1] = SRC(1)[1]; def->ctrl[0][1].xyz[2] = SRC(1)[2];
        def->ctrl[1][1].xyz[0] = SRC(1)[0]; def->ctrl[1][1].xyz[1] = SRC(1)[1]; def->ctrl[1][1].xyz[2] = SRC(1)[2];
        def->ctrl[2][1].xyz[0] = SRC(2)[0]; def->ctrl[2][1].xyz[1] = SRC(2)[1]; def->ctrl[2][1].xyz[2] = SRC(2)[2];
        def->ctrl[3][1].xyz[0] = SRC(3)[0]; def->ctrl[3][1].xyz[1] = SRC(3)[1]; def->ctrl[3][1].xyz[2] = SRC(3)[2];
        def->ctrl[4][1].xyz[0] = SRC(3)[0]; def->ctrl[4][1].xyz[1] = SRC(3)[1]; def->ctrl[4][1].xyz[2] = SRC(3)[2];
        def->ctrl[0][2].xyz[0] = SRC(1)[0]; def->ctrl[0][2].xyz[1] = SRC(1)[1]; def->ctrl[0][2].xyz[2] = SRC(1)[2];
        def->ctrl[1][2].xyz[0] = SRC(1)[0]; def->ctrl[1][2].xyz[1] = SRC(1)[1]; def->ctrl[1][2].xyz[2] = SRC(1)[2];
        def->ctrl[2][2].xyz[0] = SRC(2)[0]; def->ctrl[2][2].xyz[1] = SRC(2)[1]; def->ctrl[2][2].xyz[2] = SRC(2)[2];
        def->ctrl[3][2].xyz[0] = SRC(3)[0]; def->ctrl[3][2].xyz[1] = SRC(3)[1]; def->ctrl[3][2].xyz[2] = SRC(3)[2];
        def->ctrl[4][2].xyz[0] = SRC(3)[0]; def->ctrl[4][2].xyz[1] = SRC(3)[1]; def->ctrl[4][2].xyz[2] = SRC(3)[2];
        bFlipMatch = 1;   // nType==3 takes the shared else block's flip flag (1)
    }
    #undef SRC

    // vertical-flip the cap grid when bFirst matches this shape's flip flag.
    if ( bFirst == bFlipMatch )
    {
        for ( int col = 0; col < def->width; ++col )
            for ( int r = 0; r < def->height / 2; ++r )
            {
                drawVert_t tmp                      = def->ctrl[col][r];
                def->ctrl[col][r]                   = def->ctrl[col][def->height - 1 - r];
                def->ctrl[col][def->height - 1 - r] = tmp;
            }
    }

    Patch_Rebuild( def, 1 );
    {
        const int layer = g_qeglobals.current_edit_layer;
        float s = g_qeglobals.random_texture_stuff[layer].sampleSize;
        Patch_CapTexture( def, 0, s, s );
    }
    iassert( def->pSymbiot == (entity_brush_s *)b->def );
    iassert( b->patch->def == def );
    iassert( b->def->patch == def );
    return b;
}

// UI-independent action behind CCapDialog's OK button (inside Patch_CapCurrent in the binary).
int Cap_Apply( patchMesh_t *def, int nType, selbrush_t **caps )
{
    selbrush_t *c0 = Patch_CapSpecial( def, nType, 0 );
    selbrush_t *c1 = Patch_CapSpecial( def, nType, 1 );
    caps[0] = c0;
    caps[1] = c1;
    return 2;
}

// ── CCapDialog (0x40A8A0, IDD 0xA1 = 161) — the "special cap" type picker ──────
//   Radio dialog popped by Patch_CapCurrent when the patch has no closed seam edge.
//   Its DoDataExchange (0x40A900) is one DDX_Radio(pDX, 1285, m_nType)
//   over the 4-button group {1285 Bevel, 1287 Endcap, 1289 Inverted Bevel, 1291
//   Inverted Endcap} → index 0..3.  KIWI: imgui_panel_cap.cpp is that dialog, over
//   Cap_Commit below.

// The closed edges of a patch - opposite control-grid edges that coincide (a closed loop:
// cap it) - as (bByColumn, bLast) pairs in the binary's order.  Returns the count.
static int Patch_CapSeams( patchMesh_t *def, int seams[4][2] )
{
    int n = 0;
    //  1) ctrl[0][0]        vs ctrl[width-1][0]   (column 0 vs last column, row 0)
    if ( Vec3NearEqual( def->ctrl[0][0].xyz, def->ctrl[def->width - 1][0].xyz ) )
        { seams[n][0] = 1; seams[n][1] = 0; ++n; }
    //  2) ctrl[0][height-1] vs ctrl[width-1][height-1]
    if ( Vec3NearEqual( def->ctrl[0][def->height - 1].xyz,
                        def->ctrl[def->width - 1][def->height - 1].xyz ) )
        { seams[n][0] = 1; seams[n][1] = 1; ++n; }
    //  3) ctrl[0][0]        vs ctrl[0][height-1]  (row 0 vs last row, column 0)
    if ( Vec3NearEqual( def->ctrl[0][0].xyz, def->ctrl[0][def->height - 1].xyz ) )
        { seams[n][0] = 0; seams[n][1] = 0; ++n; }
    //  4) ctrl[width-1][0]  vs ctrl[width-1][height-1]
    if ( Vec3NearEqual( def->ctrl[def->width - 1][0].xyz,
                        def->ctrl[def->width - 1][def->height - 1].xyz ) )
        { seams[n][0] = 0; seams[n][1] = 1; ++n; }
    return n;
}

// The shared tail of both cap paths: select the caps and, for a world patch, group them
// into a "patchCapped" func_group (the Patch_CapCurrent tail).
static void Patch_CapFinish( selbrush_t *sel, selbrush_t **caps, int n )
{
    if ( n <= 0 )
        return;
    for ( int i = n - 1; i >= 0; --i )
        if ( caps[i] )
            Select_Brush( caps[i], 1, 1, 0 );

    if ( (entity_s *)sel->owner == world_entity )
    {
        eclass_t *ec = Eclass_ForName( 0, "func_group" );
        if ( ec )
        {
            entity_s *ent = Entity_Create( ec );
            SetKeyValue( (entity_s_def *)ent->def, "type", "patchCapped" );
            Undo_SetIdForEntity( (entity_s_def *)ent->def );
        }
    }
}

// KIWI: Curve > Cap's pre-check - -1 when the selection is not one patch (printed), else the
// number of closed edges.  Cmd_OnCurveCap opens its undo bracket only when there is
// something to auto-cap; an open patch (0) goes to the type prompt, which brackets its commit.
int Patch_CapCountSeams()
{
    if ( !( QE_SingleBrush() && selected_brushes_next->patch ) )
    {
        Sys_Printf( "Cannot cap multiple selection. Please select a single patch.\n" );
        return -1;
    }
    int seams[4][2];
    return Patch_CapSeams( selected_brushes_next->patch->def, seams );
}

// ── Patch_CapCurrent (0x43AA20) — Curve→Cap: cap the single selected patch ──────
//   Auto-cap every "closed" edge, each becoming a cap brush via Patch_Cap, and group the
//   caps into a func_group.  The binary popped CCapDialog when no edge was closed (open
//   patch) and built two special caps from it - KIWI: that is the ImGui prompt
//   (imgui_panel_cap.cpp -> Cap_Commit), opened by Cmd_OnCurveCap instead.
void Patch_CapCurrent()
{
    if ( !( QE_SingleBrush() && selected_brushes_next->patch ) )
    {
        Sys_Printf( "Cannot cap multiple selection. Please select a single patch.\n" );
        return;
    }

    selbrush_t   *sel = selected_brushes_next;
    iassert( sel->patch->def == sel->def->patch );
    patchMesh_t  *def = sel->patch->def;
    selbrush_t   *caps[4] = { 0, 0, 0, 0 };
    int           seams[4][2];
    const int     count = Patch_CapSeams( def, seams );
    int           n = 0;
    for ( int s = 0; s < count; ++s )
    {
        selbrush_t *c = Patch_Cap( def, seams[s][0], seams[s][1] );
        if ( c ) caps[n++] = c;
    }
    Patch_CapFinish( sel, caps, n );
}

// KIWI: the open-patch cap - CCapDialog's OK (Patch_CapCurrent 0x43abaf-0x43abf3), from the
// ImGui prompt.  Re-checks the selection (it can change while the prompt is up) and brackets
// the COMMIT, so Cancel leaves no undo record (the Sides_Commit pattern).  The Endcap types
// (1, 3) read source columns 3 and 4 - on a patch under 5 columns wide the binary built a
// garbage cap from stale control points; refused here.
void Cap_Commit( int nType )
{
    if ( !( QE_SingleBrush() && selected_brushes_next->patch ) )
    {
        Sys_Printf( "Cap: select a single patch.\n" );
        return;
    }
    selbrush_t  *sel = selected_brushes_next;
    patchMesh_t *def = sel->patch->def;
    if ( ( nType == 1 || nType == 3 ) && def->width < 5 )
    {
        Sys_Printf( "Cap: an endcap needs a patch at least 5 columns wide.\n" );
        return;
    }
    Undo_ClearRedo();
    Undo_GeneralStart( "curve cap" );
    Undo_AddBrushList( &selected_brushes );
    selbrush_t *caps[4] = { 0, 0, 0, 0 };
    const int n = Cap_Apply( def, nType, caps );
    Patch_CapFinish( sel, caps, n );
    Undo_EndBrushList( &selected_brushes );
    Undo_End();
    g_nUpdateBits = -1;
}

// ── Patch_Thicken (0x448700) — Curve→Thicken: offset a copy along normals ──────
//   bseam = build the connecting seam strip patches.  Duplicates the
//   selected patch, offsets every control point of the duplicate by (-thickness)*normal
//   (mesh normals must be computed first), inverts it, and — when bseam — builds up to
//   four 3-wide "seam" patches connecting the source edges to the offset copy.
//   Reached via OnCurveThicken (cmd 32904, CDialogThick IDD 0xA4).
extern patchMesh_t *Patch_Duplicate( patchMesh_t *from );
static void patchInvert2( patchMesh_t *p );   // fwd (defined above; static)

// One Thicken seam strip: a 3-wide patch from the source's edge (column `edge` when
// `alongHeight`, else row `edge`) to the offset copy's; `flip` turns it to face out.
// KIWI: one helper for the four copies, and a strip Patch_GenericMesh refuses (it printed
// why) is skipped - the binary dereferenced the NULL strip.
static void Patch_ThickenSeam( patchMesh_t *src, patchMesh_t *dup, bool alongHeight, int edge,
                               bool flip )
{
    const int n = alongHeight ? src->height : src->width;
    selbrush_t *b = nullptr;
    if ( alongHeight && n < 3 )
        Sys_Printf( "Invalid patch width or height.\n" );
    else
        b = alongHeight ? Patch_GenericMesh( 3, n, 2, 0, 1 ) : Patch_GenericMesh( n, 3, 2, 0, 1 );
    if ( !b )
        return;
    iassert( b->patch->def == b->def->patch );
    patchMesh_t *def = b->patch->def;
    def->type = (PATCH_TYPES)( def->type | PATCH_SEAM );
    for ( int i = 0; i < n; ++i )
    {
        // strip index 0 = the source edge, 2 = the copy's edge, 1 = their midpoint
        const drawVert_t &s = alongHeight ? src->ctrl[edge][i] : src->ctrl[i][edge];
        const drawVert_t &d = alongHeight ? dup->ctrl[edge][i] : dup->ctrl[i][edge];
        drawVert_t &v0 = alongHeight ? def->ctrl[0][i] : def->ctrl[i][0];
        drawVert_t &v1 = alongHeight ? def->ctrl[1][i] : def->ctrl[i][1];
        drawVert_t &v2 = alongHeight ? def->ctrl[2][i] : def->ctrl[i][2];
        for ( int k = 0; k < 3; ++k )
        {
            v0.xyz[k] = s.xyz[k];
            v2.xyz[k] = d.xyz[k];
            v1.xyz[k] = ( v0.xyz[k] + v2.xyz[k] ) * 0.5f;
        }
    }
    float smins[3], smaxs[3];
    Patch_CalcBounds( smins, smaxs, def, true );
    Brush_RebuildBrush( (brush_t *)def->pSymbiot, smins, smaxs );
    Patch_Naturalize( def );
    if ( flip )
    {
        patchInvert2( def );
        if ( def->curveDef )
            free( def->curveDef );
        def->curveDef = Patch_GenericMesh2( def, g_qeglobals.current_edit_layer, 0, 0 );
        ++def->version;
    }
    Select_Brush( b, 1, 1, 0 );
}

void Patch_Thicken( int thickness, char bseam )
{
    int negThick = -thickness;
    if ( !( QE_SingleBrush() && selected_brushes_next->patch
            && selected_brushes_next->patch->def ) )
    {
        Sys_Printf( "Cannot thicken multiple patches. Please select a single patch.\n" );
        return;
    }
    patchMesh_t *src = selected_brushes_next->patch->def;

    if ( ( src->type & PATCH_TERRAIN ) != 0 )
    {
        Sys_Printf( "Thickening of terrain patches not yet implemented.  "
                    "Tell the coders if you need this feature.\n" );
        return;
    }

    Undo_ClearRedo();
    Undo_GeneralStart( "Patch thicken" );
    Undo_AddBrushList( &selected_brushes );
    Patch_MeshNormals( src );
    patchMesh_t *dup = Patch_Duplicate( src );     // the offset copy

    // 1) offset every control point of the copy: dup.xyz = src.xyz + (-t)*src.normal
    float t = (float)negThick;
    for ( int col = 0; col < src->width; ++col )
        for ( int row = 0; row < src->height; ++row )
        {
            drawVert_t *s = &src->ctrl[col][row];
            drawVert_t *d = &dup->ctrl[col][row];
            d->xyz[0] = s->normal[0] * t + s->xyz[0];
            d->xyz[1] = s->normal[1] * t + s->xyz[1];
            d->xyz[2] = s->normal[2] * t + s->xyz[2];
        }

    Patch_Rebuild( dup, 1 );
    dup->type = (PATCH_TYPES)( dup->type | PATCH_THICK );
    selbrush_t *inst = (selbrush_t *)Brush_AddToList( (brush_t *)dup->pSymbiot,
                                                      selected_brushes_next->owner );
    if ( inst->next || inst->prev )
        Com_Error( ERR_FATAL, "Brush_AddToList: already linked" );
    Brush_AddToList2( inst );

    if ( bseam )
    {
        const int lastCol = src->width - 1, lastRow = src->height - 1;
        // Non-cylinder patches get four seam strips (two along height, two along width).
        if ( ( src->type & PATCH_CYLINDER ) == 0 )
        {
            Patch_ThickenSeam( src, dup, true, 0, true );          // A: column 0
            Patch_ThickenSeam( src, dup, true, lastCol, false );   // B: last column
        }
        Patch_ThickenSeam( src, dup, false, 0, true );             // C: row 0
        Patch_ThickenSeam( src, dup, false, lastRow, false );      // D: last row
    }

    // invert the offset copy so its faces point outward, rebuild its curveDef.
    patchInvert2( dup );
    if ( dup->curveDef )
        free( dup->curveDef );
    dup->curveDef = Patch_GenericMesh2( dup, g_qeglobals.current_edit_layer, 0, 0 );
    ++dup->version;

    if ( PatchDialog_IsOpen() )
        g_PatchDialog_GetPatchInfo();

    Undo_EndBrushList( &selected_brushes );
    Undo_End();
}

// ═════════════════════════════════════════════════════════════════════════════
//  WELD CLUSTER — Curve→Weld (WeldMesh) joins two patches sharing an edge into one.
// ═════════════════════════════════════════════════════════════════════════════

// ── patchInvert (0x446370) — HORIZONTAL FLIP: mirror each row's columns ────────
//   Counterpart to patchInvert2 (which flips vertically).  Swaps ctrl[col][row] with
//   ctrl[width-1-col][row] for col in [0, width/2) over every row; terrain patches then
//   propagate each control vertex's turned_edge bit0 across adjacent columns.
static void patchInvert( patchMesh_t *p )
{
    for ( int row = 0; row < p->height; ++row )
    {
        for ( int c = 0; c < p->width / 2; ++c )
        {
            drawVert_t tmp                 = p->ctrl[c][row];
            p->ctrl[c][row]                = p->ctrl[p->width - 1 - c][row];
            p->ctrl[p->width - 1 - c][row] = tmp;
        }
        if ( ( p->type & PATCH_TERRAIN ) != 0 )
        {
            for ( int c = 0; c < p->width - 1; ++c )
                p->ctrl[c][row].turned_edge ^=
                    ( p->ctrl[c][row].turned_edge ^ p->ctrl[c + 1][row].turned_edge ) & 1;
        }
    }
}

// ── Patch_Weld_EdgeParams (0x44C1A0) — pick edge-walk start/step ────
//   Given an edge selector (fixedCol, atEnd, reverse) fills the start (outStartCol,
//   outStartRow) and step (outStepCol, outStepRow) to iterate over one edge of a patch
//   grid `pm`.
static void Patch_Weld_EdgeParams( int *outStartCol, const patchMesh_t *pm,
                                   int *outStepCol, int *outStartRow,
                                   int fixedCol, int atEnd, int reverse, int *outStepRow )
{
    if ( fixedCol )
    {
        if ( atEnd ) *outStartCol = pm->width - 1;
        else         *outStartCol = 0;
        *outStepCol = 0;
        if ( reverse )
        {
            *outStartRow = pm->height - 1;
            *outStepRow  = -1;
        }
        else
        {
            *outStartRow = 0;
            *outStepRow  = 1;
        }
    }
    else
    {
        if ( reverse )
        {
            *outStartCol = pm->width - 1;
            *outStepCol  = -1;
        }
        else
        {
            *outStartCol = 0;
            *outStepCol  = 1;
        }
        if ( atEnd ) *outStartRow = pm->height - 1;
        else         *outStartRow = 0;
        *outStepRow = 0;
    }
}

// ── Patch_WeldStitch (0x44C280) — stitch pm1 onto pm0 along the matched edge ──
//   pb0Inst = the surviving brush instance (its DEF's patch becomes the merged patch);
//   pb1Inst the consumed one.  transpose/invert/flip carry the {fixedCol, atEnd, reverse}
//   state of each patch's matched edge (from Patch_Weld's search).  Transforms both grids
//   into a canonical orientation (transpose/invert as needed), appends pm1's columns onto
//   pm0, restores pm0's orientation, then frees pb1's brush (or its owning entity).
static void Patch_WeldStitch( selbrush_t *pb0Inst, selbrush_t *pb1Inst,
                              const int *transpose, const int *invert, const int *flip )
{
    iassert( pb1Inst->patch->def == pb1Inst->def->patch );
    // pm0 is reached through pb0's def brush (the same patch->def == def->patch invariant).
    brush_t     *pb0Def = pb0Inst->def;
    patchMesh_t *pm0    = pb0Def->patch;
    patchMesh_t *pm1    = pb1Inst->patch->def;

    int inv0 = 1 - invert[0];
    if ( transpose[0] != 1 )
        Patch_TransposeGrid( pm0 );
    if ( inv0 != 1 )
        patchInvert( pm0 );
    if ( flip[0] )
        patchInvert2( pm0 );

    char didTranspose1 = 0;
    char didInvert1     = 0;
    if ( transpose[1] != 1 ) { didTranspose1 = 1; Patch_TransposeGrid( pm1 ); }
    if ( invert[1] != 1 )    { didInvert1    = 1; patchInvert( pm1 ); }
    if ( flip[1] )           { didInvert1    = !didInvert1; patchInvert2( pm1 ); }

    iassert( pm0->height == pm1->height );

    // sanity: pm1's LAST column must coincide with pm0's FIRST column (shared edge).
    for ( int j = 0; j < pm0->height; ++j )
        iassert( Vec3NearEqual( pm1->ctrl[pm1->width - 1][j].xyz, pm0->ctrl[0][j].xyz ) );

    // append pm1's columns 1..width-1 after pm0's existing columns (col0 is the shared
    // edge).  First OR pm1 col0's turned_edge bit0 into pm0's LAST column edge bit.
    for ( int j = 0; j < pm0->height; ++j )
    {
        drawVert_t *pm0Edge = &pm0->ctrl[pm0->width - 1][j];
        pm0Edge->turned_edge &= ~1;
        pm0Edge->turned_edge |= ( pm1->ctrl[0][j].turned_edge & 1 );
        for ( int i = 1; i < pm1->width; ++i )
            pm0->ctrl[pm0->width - 1 + i][j] = pm1->ctrl[i][j];
    }
    pm0->width += pm1->width - 1;

    if ( didTranspose1 )
        Patch_TransposeGrid( pm0 );
    if ( didInvert1 )
    {
        patchInvert2( pm0 );
        if ( pm0->curveDef )
            free( pm0->curveDef );
        pm0->curveDef = Patch_GenericMesh2( pm0, g_qeglobals.current_edit_layer, 0, 0 );
        ++pm0->version;
    }
    Patch_Rebuild( pm0, 1 );

    entity_s *owner = pb1Inst->owner;
    if ( owner != world_entity
      && owner->brushes.ownerNext == pb1Inst
      && owner->brushes.ownerPrev == pb1Inst )
    {
        Undo_AddEntity_W( (entity_s *)owner->def );
        Entity_Free( pb1Inst->owner );
    }
    else
    {
        Brush_Free( pb1Inst );
    }
}

// ── Patch_Weld (0x44C590) — try to weld patch instance pb0 onto pb1 ───────────
//   Searches all 2x2x2 edge orientations of both patches for a coincident edge (same
//   length + coincident control points).  If EXACTLY ONE edge pair matches (more than
//   one → refuse), and the merged edge count fits in MAX_PATCH_WIDTH (16), stitches
//   them via Patch_WeldStitch.  Returns 1 on a successful weld, 0 otherwise.
static char Patch_Weld( selbrush_t *pb0, selbrush_t *pb1 )
{
    iassert( pb0 );
    iassert( pb1 );
    iassert( pb0 != pb1 );
    iassert( pb0->patch->def == pb0->def->patch );
    iassert( pb1->patch->def == pb1->def->patch );

    patchMesh_t *pm0 = pb0->patch->def;
    patchMesh_t *pm1 = pb1->patch->def;

    char foundMatch = 0;
    int  edgeCol[2], edgeEnd[2], edgeRev[2];        // matched edge of pm0 [0] / pm1 [1]

    for ( int e0 = 0; e0 < 2; ++e0 )                // byColumn of pm0's edge
    {
        int len0 = e0 ? pm0->height : pm0->width;   // height along e0's edge
        for ( int e1 = 0; e1 < 2; ++e1 )            // byColumn of pm1's edge
        {
            int len1 = e1 ? pm1->height : pm1->width;
            if ( len0 != len1 )
                continue;

            // try the two mirror orientations of pm0's edge and of pm1's.
            for ( int f0 = 0; f0 < 2; ++f0 )
            {
                int startCol0, stepCol0, startRow0, stepRow0;
                Patch_Weld_EdgeParams( &startCol0, pm0, &stepCol0, &startRow0,
                                       e0, f0, 0, &stepRow0 );

                for ( int f1 = 0; f1 < 2; ++f1 )
                {
                    for ( int rev = 0; rev < 2; ++rev )   // reverse pm1's walk
                    {
                        int startCol1, stepCol1, startRow1, stepRow1;
                        Patch_Weld_EdgeParams( &startCol1, pm1, &stepCol1, &startRow1,
                                               e1, f1, rev, &stepRow1 );

                        // walk both edges in lockstep, comparing each control point.
                        int c0 = startCol0, r0 = startRow0;
                        int c1 = startCol1, r1 = startRow1;
                        int matched = 0;
                        int i;
                        for ( i = 0; i < len0; ++i )
                        {
                            if ( !Vec3NearEqual( pm0->ctrl[c0][r0].xyz, pm1->ctrl[c1][r1].xyz ) )
                                break;              // this point differed → edge mismatch
                            c0 += stepCol0; r0 += stepRow0;
                            c1 += stepCol1; r1 += stepRow1;
                            ++matched;
                        }
                        if ( matched != len0 )
                            continue;               // whole edge did not coincide

                        // edge coincides.  Refuse if a second match was already found.
                        if ( foundMatch )
                        {
                            Sys_Printf( "Cannot weld patches; more than one edge matches\n" );
                            return 0;
                        }
                        // new merged edge count must fit MAX_PATCH_WIDTH (16).
                        int newEdge = pm1->width + pm0->height + pm1->height
                                    - 2 * len0 + pm0->width - 1;
                        if ( newEdge > 16 )
                        {
                            Sys_Printf( "Cannot weld patches; new edge size of %i is bigger than %i\n",
                                        newEdge, 16 );
                            return 0;
                        }
                        edgeCol[0] = e0;  edgeCol[1] = e1;
                        edgeEnd[0] = f0;  edgeEnd[1] = f1;
                        edgeRev[0] = 0;   edgeRev[1] = rev;
                        foundMatch = 1;
                    }
                }
            }
        }
    }

    if ( !foundMatch )
        return 0;
    Patch_WeldStitch( pb1, pb0, edgeCol, edgeEnd, edgeRev );
    return 1;
}

// ── WeldMesh (0x44C8C0) — Curve→Weld: weld all adjacent selected patches ───────
//   Every selected brush must be a patch, and they must all belong to the same
//   worldspawn-ish owner (or all be worldspawn func_groups).  Repeatedly welds any two
//   selected patches sharing an edge until no more welds occur.

char WeldMesh()
{
    entity_s *commonOwner = 0;
    char sawGroup = 0;                             // worldspawn-ish
    char sawEntity = 0;                            // non-worldspawn

    // validate the selection: all patches, consistent ownership.
    selbrush_t *b = selected_brushes_next;
    if ( b != &selected_brushes )
    {
        while ( b->patch )
        {
            const char *value = (const char *)zero;
            epair_t *epairs = ((entity_s *)b->owner->def)->epairs;
            if ( epairs )
            {
                while ( _stricmp( epairs->key, "classname" ) )
                {
                    epairs = epairs->next;
                    if ( !epairs ) { value = (const char *)zero; goto haveValue; }
                }
                value = epairs->value;
            }
        haveValue:
            if ( Classname_IsWeldContainer( value ) )             // worldspawn/func_group/func_cullgroup
            {
                if ( sawEntity )
                    return 0;
                sawGroup = 1;
            }
            else
            {
                if ( sawGroup || ( commonOwner && commonOwner != b->owner ) )
                    return 0;
                sawEntity = 1;
            }
            entity_s *o = b->owner;
            b = b->next;
            commonOwner = o;
            if ( b == &selected_brushes )
                goto doWeld;
        }
        return 0;                                  // a non-patch was selected
    }

doWeld:
    Undo_ClearRedo();
    Undo_GeneralStart( "weld meshes" );
    Undo_AddBrushList( &selected_brushes );

    // O(n^2) pairwise weld: for each i, try to weld a later `next` onto it.  On a
    // successful weld the whole outer scan restarts, because the merged patch changed
    // the list.
restartWeld:
    for ( selbrush_t *i = selected_brushes_next; i != &selected_brushes; i = i->next )
    {
        selbrush_t *next = i->next;
        if ( next == &selected_brushes )
            continue;
        while ( ( ( (byte)i->def->patch->type
                    ^ (byte)next->def->patch->type ) & 0x40 ) != 0
                || !Patch_Weld( next, i ) )
        {
            next = next->next;
            if ( next == &selected_brushes )
                goto nextI;                        // this i had no match
        }
        goto restartWeld;                          // welded → restart the outer scan
    nextI:;
    }

    Undo_EndBrushList( &selected_brushes );
    Undo_End();
    if ( PatchDialog_IsOpen() )
        g_PatchDialog_GetPatchInfo();
    g_nUpdateBits = -1;
    return 1;
}

// ═════════════════════════════════════════════════════════════════════════════
//  SPLIT CLUSTER — Curve→Split (SplitPatch) splits a patch at a selected mid-edge.
// ═════════════════════════════════════════════════════════════════════════════

// ── Patch_SelectedRowOrCol (0x43BFE0) — resolve the selected verts' row/col ──
//   Scans g_qeglobals.d_move_points for control points that belong to `patch`, returns
//   their common column (*px) and row (*py) — one of which is a real index and the other
//   -1 (the selection lies on a single row or a single column).  0 if the selection
//   is empty, spans both axes, or none of the points belong to this patch.
static char Patch_SelectedRowOrCol( patchMesh_t *patch, int *px, int *py )
{
    iassert( patch );
    iassert( px );
    iassert( py );

    *px = -1;
    *py = -1;
    if ( g_qeglobals.d_num_move_points <= 0 )
        return 0;

    char have = 0;
    for ( int idx = 0; idx < g_qeglobals.d_num_move_points; ++idx )
    {
        drawVert_t *dv = (drawVert_t *)g_qeglobals.d_move_points[idx];
        // in-grid test: dv within [&ctrl[0][0], &ctrl[15][15]].
        if ( dv >= &patch->ctrl[0][0] && dv <= &patch->ctrl[15][15] )
        {
            // KIWI: pointer difference; the binary's `- 56` is x86's ctrl offset.
            int flat = (int)( dv - &patch->ctrl[0][0] );
            int col  = flat / 16;
            int row  = flat % 16;
            iassert( dv == &patch->ctrl[col][row] );
            if ( have )
            {
                if ( *px != col ) *px = -1;
                if ( *py != row ) *py = -1;
            }
            else
            {
                *px  = col;
                *py  = row;
                have = 1;
            }
        }
    }
    if ( !have )
        return 0;
    if ( *px < 0 )
    {
        if ( *py < 0 )
        {
            Sys_Printf( "selected vertices must be on the same column or row\n" );
            return 0;
        }
    }
    else if ( *py >= 0 )
        return 0;
    return 1;
}

// ── Patch_SplitAt (0x44CA70) — split off a sub-patch at (col,row) ──────
//   Copies the control-grid region from (col,row)..(width,height) of `pm` into a new
//   patch (reusing pm's materials/type), adds a symbiont brush owned by owner, then
//   trims pm to end at the split (width=col+1 OR height=row+1).  Exactly one of
//   col/row is nonzero (the other must be 0 — the split axis).
static void Patch_SplitAt( patchMesh_t *pm, int col, int row, entity_s *owner )
{
    iassert( col == 0 || row == 0 );

    patchMesh_t *NewPatch = MakeNewPatch();
    // copy the 3 material channels ({lyrMtl, radMtl} pairs) from pm.
    NewPatch->texture   = pm->texture;
    NewPatch->lightmap  = pm->lightmap;
    NewPatch->smoothing = pm->smoothing;

    NewPatch->width               = pm->width  - col;
    NewPatch->height              = pm->height - row;
    NewPatch->contents            = pm->contents;
    NewPatch->type                = pm->type;
    NewPatch->subDivType          = pm->subDivType;
    NewPatch->size_of_struct_0x504C = pm->size_of_struct_0x504C;
    NewPatch->bDirty              = pm->bDirty;

    for ( int c = col; c < pm->width; ++c )
        for ( int r = row; r < pm->height; ++r )
            NewPatch->ctrl[c - col][r - row] = pm->ctrl[c][r];

    brush_t *nb = AddBrushForPatch( NewPatch, (entity_s *)owner->def );
    Patch_Rebuild( NewPatch, 1 );
    iassert( nb->owner == (entity_s *)pm->pSymbiot->owner );
    iassert( nb->owner == (entity_s *)owner->def );

    selbrush_t *inst = (selbrush_t *)Brush_AddToList( nb, owner );
    if ( inst->next || inst->prev )
        Com_Error( ERR_FATAL, "Brush_AddToList: already linked" );
    Brush_AddToList2( inst );

    if ( col )
        pm->width = col + 1;
    else
        pm->height = row + 1;
    Patch_Rebuild( pm, 1 );
}

// ── SplitPatch (0x44CC60) — Curve→Split: split selected patches at a mid-edge ──
//   For each selected patch with verts selected on a mid row/column (odd index for a
//   non-terrain patch), split it into two patches at that seam (Patch_SplitAt),
//   then restart the scan.  Reached via OnSplitPatch (cmd 33158).
void SplitPatch()
{
    Undo_ClearRedo();
    Undo_GeneralStart( "split patches" );
    Undo_AddBrushList( &selected_brushes );

    char didSplit = 0;
restartSplit:
    for ( selbrush_t *i = selected_brushes_next; i != &selected_brushes; i = i->next )
    {
        patch_t *patch = i->patch;
        if ( !patch )
            continue;
        iassert( patch->def == i->def->patch );
        patchMesh_t *def = i->patch->def;
        int selCol, selRow;
        if ( !Patch_SelectedRowOrCol( def, &selCol, &selRow ) )
            continue;
        if ( selCol < 0 )
        {
            // split along a ROW (selRow); must be an interior, splittable row.
            if ( selRow && selRow != def->height - 1 )
            {
                if ( ( def->type & 0x40 ) != 0 || ( selRow & 1 ) == 0 )
                {
                    Patch_SplitAt( def, 0, selRow, (entity_s *)(intptr_t)i->owner );
                    didSplit = 1;
                    goto restartSplit;
                }
                Sys_Printf( "can only split curves on odd rows and columns" );
            }
        }
        else if ( selCol && selCol != def->width - 1 )
        {
            if ( ( def->type & 0x40 ) != 0 || ( selCol & 1 ) == 0 )
            {
                Patch_SplitAt( def, selCol, 0, (entity_s *)(intptr_t)i->owner );
                didSplit = 1;
                goto restartSplit;
            }
            Sys_Printf( "can only split curves on odd rows and columns" );
        }
    }

    Undo_EndBrushList( &selected_brushes );
    Undo_End();
    if ( didSplit )
    {
        if ( PatchDialog_IsOpen() )
            g_PatchDialog_GetPatchInfo();
        g_nUpdateBits = -1;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  TERRAIN ROW/COLUMN OPERATION CLUSTER
// ═════════════════════════════════════════════════════════════════════════════

// terrain single-row/col helpers already exist (Patch_TerrainInsert/RemoveRow/Column).
// Deps declared elsewhere: Patch_SelectedRowOrCol (above), Patch_Rebuild, etc.

extern texdef_sub_t *MaterialDef_FindTexdef( MaterialDef *mtlDef, int layerHandle );   // brush.cpp 0x45D360
#define active_brushes_next ( active_brushes.next )
extern void  Brush_SnapToGrid( brush_t *b );                               // brush.cpp 0x4783D0

// ── Patch_SelectedVertPair (0x43BE70) — resolve a selected-vertex PAIR to a row/col ─
//   Finds the (up to two) selected control points that belong to `patch`; fills the
//   pair struct pPair = {int x[2]; int y[2]} with their (col,row).  Returns 1 iff
//   exactly two points were found on the same row OR the same column.
static char Patch_SelectedVertPair( patchMesh_t *patch, char *pPair )
{
    iassert( patch );
    iassert( pPair );

    drawVert_t *found[2] = { 0, 0 };
    if ( g_qeglobals.d_num_move_points <= 0 )
        return 0;
    for ( int i = 0; i < g_qeglobals.d_num_move_points; ++i )
    {
        drawVert_t *dv = g_qeglobals.d_move_points[i];
        if ( (uintptr_t)dv >= (uintptr_t)&patch->ctrl[0][0]
          && (uintptr_t)dv <= (uintptr_t)&patch->ctrl[15][15] )
        {
            if ( found[0] )
            {
                if ( found[1] )
                    return 0;               // more than two → ambiguous
                found[1] = dv;
            }
            else
                found[0] = dv;
        }
    }
    if ( !found[1] )
        return 0;

    int *pair = (int *)pPair;               // pPair->x[0], x[1], y[0], y[1] laid out {x0,x1,y0,y1}
    for ( int i = 0; i < 2; ++i )
    {
        size_t offset = (uintptr_t)found[i] - (uintptr_t)&patch->ctrl[0][0];
        int flat = (int)( offset / sizeof(drawVert_t) );
        pair[i]     = flat / 16;            // x[i] = col
        pair[i + 2] = flat % 16;            // y[i] = row
        const bool exact = found[i] == &patch->ctrl[pair[i]][pair[i + 2]];
        iassert( exact );
        if ( !exact )
            return 0;
    }
    if ( pair[0] == pair[1] || pair[2] == pair[3] )   // same col OR same row
        return 1;
    Sys_Printf( "selected vertices must be on the same column or row\n" );
    return 0;
}

// ── Patch_TerrainExtrudeColumn (0x442DE0) — extrapolate a new column ─
//   Extends a terrain patch by ONE column at `col`, mirroring the edge:
//   new = 2*edge - inner.  Adds each new control point to the d_points overlay so it can
//   be picked.
static void Patch_TerrainExtrudeColumn( patchMesh_t *p, int col )
{
    int width = p->width;
    if ( width + 1 > 16 )
        return;

    if ( (double)width * 0.5 < (double)col || col == width - 1 )
    {
        // extrude at the HIGH edge: new column at `width` = 2*col[width-1] - col[width-2].
        for ( int row = 0; row < p->height; ++row )
        {
            p->ctrl[width][row] = p->ctrl[width - 1][row];  // copy header/materials
            for ( int k = 0; k < 3; ++k )
                p->ctrl[width][row].xyz[k] =
                    p->ctrl[width - 1][row].xyz[k] - p->ctrl[width - 2][row].xyz[k]
                  + p->ctrl[width - 1][row].xyz[k];
            for ( int k = 0; k < 3; ++k )
                g_qeglobals.d_points[g_qeglobals.d_numpoints][k] = p->ctrl[width][row].xyz[k];
            if ( g_qeglobals.d_numpoints < 2047 )
                ++g_qeglobals.d_numpoints;
        }
        ++p->width;
    }
    else
    {
        // extrude at the LOW edge: shift all columns right, new column 0 = 2*col0-col1.
        for ( int c = width; c > 0; --c )
            for ( int row = 0; row < p->height; ++row )
                p->ctrl[c][row] = p->ctrl[c - 1][row];
        for ( int row = 0; row < p->height; ++row )
        {
            for ( int k = 0; k < 3; ++k )
                p->ctrl[0][row].xyz[k] =
                    p->ctrl[1][row].xyz[k] - p->ctrl[2][row].xyz[k]
                  + p->ctrl[1][row].xyz[k];
            for ( int k = 0; k < 3; ++k )
                g_qeglobals.d_points[g_qeglobals.d_numpoints][k] = p->ctrl[0][row].xyz[k];
            if ( g_qeglobals.d_numpoints < 2047 )
                ++g_qeglobals.d_numpoints;
        }
        ++p->width;
    }
}

// ── Patch_TerrainExtrudeRow (0x443040) — extrapolate a new row ──────
//   Extends a terrain patch by ONE row at `row`, mirroring the edge: new = 2*edge - inner.
static void Patch_TerrainExtrudeRow( patchMesh_t *p, int row )
{
    int height = p->height;
    if ( height + 1 > 16 )
        return;

    if ( (double)height * 0.5 < (double)row || row == height - 1 )
    {
        // extrude at the HIGH edge: new row at `height` = 2*row[height-1] - row[height-2].
        for ( int col = 0; col < p->width; ++col )
        {
            p->ctrl[col][height] = p->ctrl[col][height - 1];
            for ( int k = 0; k < 3; ++k )
                p->ctrl[col][height].xyz[k] =
                    p->ctrl[col][height - 1].xyz[k] - p->ctrl[col][height - 2].xyz[k]
                  + p->ctrl[col][height - 1].xyz[k];
            for ( int k = 0; k < 3; ++k )
                g_qeglobals.d_points[g_qeglobals.d_numpoints][k] = p->ctrl[col][height].xyz[k];
            if ( g_qeglobals.d_numpoints < 2047 )
                ++g_qeglobals.d_numpoints;
        }
        ++p->height;
    }
    else
    {
        // extrude at the LOW edge: shift all rows up, new row 0 = 2*row0 - row1.
        for ( int r = height; r > 0; --r )
            for ( int col = 0; col < p->width; ++col )
                p->ctrl[col][r] = p->ctrl[col][r - 1];
        for ( int col = 0; col < p->width; ++col )
        {
            for ( int k = 0; k < 3; ++k )
                p->ctrl[col][0].xyz[k] =
                    p->ctrl[col][1].xyz[k] - p->ctrl[col][2].xyz[k]
                  + p->ctrl[col][1].xyz[k];
            for ( int k = 0; k < 3; ++k )
                g_qeglobals.d_points[g_qeglobals.d_numpoints][k] = p->ctrl[col][0].xyz[k];
            if ( g_qeglobals.d_numpoints < 2047 )
                ++g_qeglobals.d_numpoints;
        }
        ++p->height;
    }
}

// ── ExtrudeTerrainRow (0x44BD40) — cmd 33192: extrude a terrain edge outward ────
//   For each selected terrain patch with a full row/column of verts selected, extrude a
//   new edge (row or column) by extrapolation.
void ExtrudeTerrainRow()
{
    char didEdit = 0;
    selbrush_t *b = selected_brushes_next;
    if ( b == &selected_brushes )
        return;
    do
    {
        patch_t *patch = b->patch;
        if ( patch )
        {
            iassert( patch->def == b->def->patch );
            patchMesh_t *def = b->patch->def;
            if ( ( def->type & PATCH_TERRAIN ) != 0 )
            {
                int px, py;
                if ( Patch_SelectedRowOrCol( def, &px, &py ) )
                {
                    if ( px < 0 )
                        Patch_TerrainExtrudeRow( def, py );
                    else
                        Patch_TerrainExtrudeColumn( def, px );
                    didEdit = 1;
                    Patch_Rebuild( def, 1 );
                }
            }
        }
        b = b->next;
    } while ( b != &selected_brushes );

    if ( didEdit )
    {
        if ( PatchDialog_IsOpen() )
            g_PatchDialog_GetPatchInfo();
        g_nUpdateBits = -1;
    }
}

// ── RemoveTerrainRowCol (0x44BE10) — cmd 33154: remove a terrain row/column ─────
//   For each selected terrain patch with a full row/column of verts selected, remove that
//   row/column.  If NO patch is selected but a single-brush entity with a targetname is,
//   its targets are re-routed and the entity is deleted (the "remove terrain entity"
//   fallback, below).
void RemoveTerrainRowCol()
{
    char didEdit = 0;
    selbrush_t *b = selected_brushes_next;
    selbrush_t *next = b;
    if ( b == &selected_brushes )
        return;

    while ( 1 )
    {
        patch_t *patch = b->patch;
        if ( patch )
        {
            iassert( patch->def == b->def->patch );
            patchMesh_t *def = b->patch->def;
            if ( ( def->type & PATCH_TERRAIN ) != 0 )
            {
                int px, py;
                if ( Patch_SelectedRowOrCol( def, &px, &py ) )
                {
                    if ( px < 0 )
                        Patch_TerrainRemoveRow( def, py );
                    else
                        Patch_TerrainRemoveColumn( def, px );
                    didEdit = 1;
                    Patch_Rebuild( def, 1 );
                    b = next;
                }
            }
        }
        else if ( selected_brushes_next->next == &selected_brushes
               && HasKeyValuePair( (entity_s_def *)b->owner->def, "targetname" ) )
        {
            // No patch: a single-brush entity with a targetname → delete it (re-routing
            // its target chain).
            const char *value;
            epair_t *epairs = ((entity_s *)b->owner->def)->epairs;
            if ( epairs )
            {
                while ( _stricmp( epairs->key, "targetname" ) )
                {
                    epairs = epairs->next;
                    if ( !epairs ) { value = (const char *)zero; goto haveTargetname; }
                }
                value = epairs->value;
            }
            else
                value = (const char *)zero;
        haveTargetname:;
            entity_s_def *entDef = (entity_s_def *)b->owner->def;
            void *newTargetName = 0;                 // holds the ValueForKey2 result
            if ( HasKeyValuePair( entDef, "target" ) )
            {
                // this entity has a "target": push its target onto everything that
                // targeted it (re-route the chain through us).
                newTargetName = ValueForKey2( entDef, "target" );
                for ( selbrush_t *ab = active_brushes_next; ab != &active_brushes; ab = ab->next )
                {
                    entity_s_def *abDef = (entity_s_def *)ab->owner->def;
                    if ( Entity_HasEpairMatch( (entity_s *)abDef, "target", value ) )
                        SetKeyValue( abDef, "target", (const char *)newTargetName );
                }
            }
            else
            {
                // no target: clear the "target" of everything that targeted us.
                for ( selbrush_t *ab = active_brushes_next; ab != &active_brushes; ab = ab->next )
                {
                    entity_s_def *abDef = (entity_s_def *)ab->owner->def;
                    if ( Entity_HasEpairMatch( (entity_s *)abDef, "target", value ) )
                    {
                        DeleteKey( &abDef->epairs, "target" );
                        Checkkey_Model( abDef, "target" );
                        Checkkey_Color( abDef, "target" );
                    }
                }
                newTargetName = 0;
            }

            Undo_ClearRedo();
            Undo_GeneralStart( "delete" );
            Undo_AddBrushList( &selected_brushes );
            for ( selbrush_t *j = selected_brushes_next; j != &selected_brushes; j = j->next )
            {
                entity_s *owner = j->owner;
                entity_s_def *ownerDef = (entity_s_def *)owner->def;
                if ( g_lastundo )
                {
                    Undo_AddEntity( (entity_s *)owner->def );
                    if ( ownerDef != (entity_s_def *)world_entity->def )
                    {
                        for ( entity_brush_s *ob = (entity_brush_s *)ownerDef->brushes.ownerPrev;
                              ob != (entity_brush_s *)&ownerDef->brushes;   // sentinel = &def->brushes
                              ob = (entity_brush_s *)ob->ownerNext )
                            Undo_AddBrush( ob );
                    }
                }
                else
                    Sys_Printf( "Undo_AddEntity: no last undo.\n" );
            }
            Select_Delete();
            Undo_EndBrushList( &selected_brushes );
            Undo_End();
            if ( PatchDialog_IsOpen() )
                g_PatchDialog_GetPatchInfo();

            // re-select everything that carries the pushed targetname (the new head).
            const char *tn = (const char *)newTargetName;
            if ( tn )
            {
                for ( selbrush_t *ab = active_brushes_next; ab != &active_brushes; ab = ab->next )
                {
                    if ( Entity_HasEpairMatch( (entity_s *)ab->owner->def, "targetname", tn ) )
                        Select_Brush( ab, 1, 1, 0 );
                }
            }
            g_nUpdateBits = -1;
            return;
        }
        next = b->next;
        if ( next == &selected_brushes )
            break;
        b = b->next;
    }

    if ( !didEdit )
        return;
    if ( PatchDialog_IsOpen() )
        g_PatchDialog_GetPatchInfo();
    g_nUpdateBits = -1;
}

// ── Patch_InsertRemoveFromVertPair (0x44BB90) — insert a terrain row/col at a pair ─
//   From a selected pair of ADJACENT terrain verts (same row/col, adjacent index),
//   insert a new row or column between them (via Patch_SelectedVertPair).
void Patch_InsertRemoveFromVertPair()
{
    char didEdit = 0;
    selbrush_t *b = selected_brushes_next;
    selbrush_t *next = b;
    if ( b == &selected_brushes )
        return;

    patchMesh_t *def = 0;
    int pair[4] = { 0, 0, 0, 0 };       // {x0, x1, y0, y1}
    while ( 1 )
    {
        patch_t *patch = b->patch;
        if ( patch )
        {
            iassert( patch->def == b->def->patch );
            def = b->patch->def;
            if ( ( def->type & PATCH_TERRAIN ) != 0
              && Patch_SelectedVertPair( def, (char *)pair ) )
                break;                  // found a valid pair → do the insert below
        }
        next = b->next;
        if ( next == &selected_brushes )
        {
            if ( didEdit )
            {
                if ( PatchDialog_IsOpen() )
                    g_PatchDialog_GetPatchInfo();
                g_nUpdateBits = -1;
            }
            return;
        }
        b = b->next;
    }

    // pair {x0,x1,y0,y1}: insert between the two adjacent verts.
    int x0 = pair[0], x1 = pair[1], y0 = pair[2], y1 = pair[3];
    if ( x0 == x1 )                     // same column → the pair differs in row → insert a ROW
    {
        if ( y0 == y1 + 1 )
            Patch_TerrainInsertRow( def, y0 );
        else if ( y1 == y0 + 1 )
            Patch_TerrainInsertRow( def, y1 );
        else
            Sys_Printf( "vertices must be adjacent to insert a row or column\n" );
    }
    else                               // differ in column → insert a COLUMN
    {
        iassert( y0 == y1 );
        if ( x0 == x1 + 1 )
            Patch_TerrainInsertColumn( def, x0 );
        else if ( x1 == x0 + 1 )
            Patch_TerrainInsertColumn( def, x1 );
        else
            Sys_Printf( "vertices must be adjacent to insert a row or column\n" );
    }
    // rebuild the tessellated curveDef.
    if ( def->curveDef )
        free( def->curveDef );
    def->curveDef = Patch_GenericMesh2( def, g_qeglobals.current_edit_layer, 0, 0 );
    ++def->version;

    if ( PatchDialog_IsOpen() )
        g_PatchDialog_GetPatchInfo();
    g_nUpdateBits = -1;
}

// ── Face_ToTerrainPatch (0x44D1F0) — Face→Terrain: convert one brush face to a terrain patch ─
//   ownerInst = the owning brush instance (selbrush_t*).  Builds a 2-row
//   terrain patch of width (winding->numpoints+1)/2 from the face's winding, projecting
//   each control point's texcoords through the face's 3 layer texture matrices.  Returns
//   the new brush instance (linked into active_brushes), or 0 if the strip would exceed
//   MAX_PATCH_WIDTH.  Reached via OnFaceToTerrain (cmd 36102).
brush_t *Face_ToTerrainPatch( face_t *face, selbrush_t *ownerInst )
{
    // build the 3 layer texture-projection matrices (8 floats each) from the face's
    // mtldef + plane normal (the same call Face_BuildLayerGeom uses).
    float mats[24];                             // 3 layers * 8 floats
    for ( int L = 0; L < 3; ++L )
    {
        float *td = (float *)MaterialDef_FindTexdef( &face->mtldef[L], 0 );
        Face_MoveTexture( (float *)td, face->plane.normal,
                          (float *)&mats[8 * L], (float *)( td + 2 ), td[4], td[5] );
    }

    winding_t *w = face->w;
    int numpoints = w->numpoints;
    int newWidth  = (unsigned int)( numpoints + 1 ) >> 1;
    if ( (unsigned int)newWidth > 16 )
        return 0;

    patchMesh_t *NewPatch = MakeNewPatch();
    NewPatch->width  = newWidth;
    NewPatch->height = 2;
    NewPatch->type   = PATCH_TERRAIN;

    // Row 0: walk winding points backward from the last (index numpoints-1); each maps to
    // ctrl[col][0].  Row 1: continue backward into ctrl[col][1] (cols width-1..0).
    int backIdx = numpoints - 1;

    for ( int col = 0; col < NewPatch->width; ++col )
    {
        const float *pt = w->p[backIdx];
        drawVert_t *cp = &NewPatch->ctrl[col][0];
        cp->xyz[0] = pt[0];
        cp->xyz[1] = pt[1];
        cp->xyz[2] = pt[2];
        for ( int L = 0; L < 3; ++L )
        {
            const float *m = &mats[8 * L];
            float s = pt[1] * m[1] + pt[0] * m[0] + pt[2] * m[2] + m[3];
            float t = pt[1] * m[5] + pt[0] * m[4] + pt[2] * m[6] + m[7];
            iassert( isfinite( s ) );
            iassert( isfinite( t ) );
            cp->texCoord.st[2 * L]     = s;
            cp->texCoord.st[2 * L + 1] = t;
        }
        cp->normal[0]           = face->plane.normal[0];
        cp->normal[1]           = face->plane.normal[1];
        cp->normal[2]           = face->plane.normal[2];
        *(int *)&cp->vert_color = face->packedColor;   // KIWI: face+0xE4 per IDA; was plane.dist's bits
        --backIdx;
    }

    if ( NewPatch->width - 1 > -1 )
    {
        for ( int col = NewPatch->width - 1; col >= 0; --col )
        {
            const float *pt = w->p[backIdx];
            drawVert_t *cp = &NewPatch->ctrl[col][1];
            cp->xyz[0] = pt[0];
            cp->xyz[1] = pt[1];
            cp->xyz[2] = pt[2];
            for ( int L = 0; L < 3; ++L )
            {
                const float *m = &mats[8 * L];
                float s = pt[1] * m[1] + pt[0] * m[0] + pt[2] * m[2] + m[3];
                float t = pt[1] * m[5] + pt[0] * m[4] + pt[2] * m[6] + m[7];
                iassert( isfinite( s ) );
                iassert( isfinite( t ) );
                cp->texCoord.st[2 * L]     = s;
                cp->texCoord.st[2 * L + 1] = t;
            }
            cp->normal[0]           = face->plane.normal[0];
            cp->normal[1]           = face->plane.normal[1];
            cp->normal[2]           = face->plane.normal[2];
            *(int *)&cp->vert_color = face->packedColor;
            if ( backIdx > 0 )
                --backIdx;
        }
    }

    NewPatch->texture.lyrMtl   = face->mtldef[0].lyrMtl;
    NewPatch->texture.radMtl   = face->mtldef[0].radMtl;
    NewPatch->lightmap.lyrMtl  = face->mtldef[1].lyrMtl;
    NewPatch->lightmap.radMtl  = face->mtldef[1].radMtl;
    NewPatch->smoothing.lyrMtl = face->mtldef[2].lyrMtl;
    NewPatch->smoothing.radMtl = face->mtldef[2].radMtl;
    if ( NewPatch->curveDef )
        free( NewPatch->curveDef );
    NewPatch->curveDef = Patch_GenericMesh2( NewPatch, g_qeglobals.current_edit_layer, 0, 0 );
    ++NewPatch->version;

    brush_t *nb = AddBrushForPatch( NewPatch, (entity_s *)( (entity_s *)ownerInst->owner )->def );
    selbrush_t *result = (selbrush_t *)Brush_AddToList( nb, ownerInst->owner );
    if ( result->next || result->prev )
        Com_Error( ERR_FATAL, "Brush_AddToList: already linked" );
    if ( &active_brushes == &selected_brushes )
    {
        Brush_AddToList2( result );
    }
    else
    {
        result->next = active_brushes_next;
        active_brushes_next->prev = result;
        active_brushes_next = result;
        result->prev = &active_brushes;
    }
    return (brush_t *)result;
}

// ══════════════════════════════════════════════════════════════════════════════
//  PATCH COMMAND CORES: drop-vertices / naturalize-cap / redisperse.
// ══════════════════════════════════════════════════════════════════════════════

extern edTrace_t *Trace_AllDirectionsIfFailed( float *cam_origin, edTrace_t *trace_result,
                                               float *dir, int contents );  // select.cpp 0x48DAA0

// ── Patch_RedistributeVerts (0x449930) — "drop vertices" (Shift+Alt+Ctrl+D) ────
//  For every PICKED control point, trace straight down (0,0,-1) against contents 4608
//  and, on a hit, move the point onto the hit.  Requires vertex/curve-point mode and a
//  single selected patch.
void Patch_RedistributeVerts()
{
    if ( g_qeglobals.d_select_mode != sel_vertex && g_qeglobals.d_select_mode != sel_curvepoint )
        return;

    patch_t     *patch;
    patchMesh_t *def;
    if ( !QE_SingleBrush()
         || ( patch = selected_brushes.next->patch ) == nullptr
         || ( def = patch->def ) == nullptr )
    {
        Sys_Printf( "must have a single patch selected to redistribute" );
        return;
    }

    float dir[3] = { 0.0f, 0.0f, -1.0f };
    for ( int i = 0; i < g_qeglobals.d_num_move_points; ++i )
    {
        drawVert_t *cp = g_qeglobals.d_move_points[i];
        float pos[3] = { cp->xyz[0], cp->xyz[1], cp->xyz[2] };
        edTrace_t tr;
        Trace_AllDirectionsIfFailed( pos, &tr, dir, 4608 );
        if ( tr.hit.brush )
        {
            drawVert_t *dst = g_qeglobals.d_move_points[i];
            dst->xyz[0] = dir[0] * tr.dist + pos[0];
            dst->xyz[1] = dir[1] * tr.dist + pos[1];
            dst->xyz[2] = tr.dist * dir[2] + pos[2];
        }
    }
    Patch_Rebuild( def, 1 );
    g_nUpdateBits = -1;
}

// Is this control point one of the picked (d_move_points) ones?  The binary open-codes
// this linear scan in both redisperse helpers below.
static bool Patch_PointIsPicked( const drawVert_t *cp )
{
    for ( int i = 0; i < g_qeglobals.d_num_move_points; ++i )
    {
        if ( g_qeglobals.d_move_points[i] == cp )
            return true;
    }
    return false;
}

// ── the two redisperse line passes ──────────────────
//  Each walks ONE line of the 16x16 control grid, finds the FIRST and SECOND picked
//  points on it, then linearly interpolates every picked point in between (skipping any
//  whose turned_edge bit 1 is set).  The interpolation parameter is
//  (index - first) / (second - first), which for points outside [first,second] simply
//  extrapolates (the binary does not clamp).
//    Patch_RedispColumn(def, col) walks ctrl[r][col] for r in [0, width)
//    Patch_RedispRow(def, row)    walks ctrl[row][c] for c in [0, height)
static void Patch_RedispColumn( patchMesh_t *def, int col )          // 0x4497C0
{
    bool haveFirst = false, haveSecond = false;
    int  iFirst = 0, iSecond = 0;
    float first[3] = { 0, 0, 0 }, second[3] = { 0, 0, 0 };

    for ( int r = 0; r < def->width; ++r )
    {
        const drawVert_t *cp = &def->ctrl[r][col];
        if ( !Patch_PointIsPicked( cp ) )
            continue;
        if ( haveFirst )
        {
            haveSecond = true;  iSecond = r;
            second[0] = cp->xyz[0]; second[1] = cp->xyz[1]; second[2] = cp->xyz[2];
        }
        else
        {
            haveFirst = true;   iFirst = r;
            first[0] = cp->xyz[0]; first[1] = cp->xyz[1]; first[2] = cp->xyz[2];
        }
    }
    if ( !haveFirst || !haveSecond )
        return;

    for ( int r = 0; r < def->width; ++r )
    {
        drawVert_t *cp = &def->ctrl[r][col];
        if ( !Patch_PointIsPicked( cp ) )
            continue;
        if ( ( cp->turned_edge & 2 ) != 0 )
            continue;
        const float t = (float)( (double)( r - iFirst ) / (double)( iSecond - iFirst ) );
        const float u = 1.0f - t;
        cp->xyz[0] = second[0] * t + first[0] * u;
        cp->xyz[1] = second[1] * t + first[1] * u;
        cp->xyz[2] = t * second[2] + u * first[2];
    }
}

static void Patch_RedispRow( patchMesh_t *def, int row )             // 0x449650
{
    bool haveFirst = false, haveSecond = false;
    int  iFirst = 0, iSecond = 0;
    float first[3] = { 0, 0, 0 }, second[3] = { 0, 0, 0 };

    for ( int c = 0; c < def->height; ++c )
    {
        const drawVert_t *cp = &def->ctrl[row][c];
        if ( !Patch_PointIsPicked( cp ) )
            continue;
        if ( haveFirst )
        {
            haveSecond = true;  iSecond = c;
            second[0] = cp->xyz[0]; second[1] = cp->xyz[1]; second[2] = cp->xyz[2];
        }
        else
        {
            haveFirst = true;   iFirst = c;
            first[0] = cp->xyz[0]; first[1] = cp->xyz[1]; first[2] = cp->xyz[2];
        }
    }
    if ( !haveFirst || !haveSecond )
        return;

    for ( int c = 0; c < def->height; ++c )
    {
        drawVert_t *cp = &def->ctrl[row][c];
        if ( !Patch_PointIsPicked( cp ) )
            continue;
        if ( ( cp->turned_edge & 2 ) != 0 )
            continue;
        const float t = (float)( (double)( c - iFirst ) / (double)( iSecond - iFirst ) );
        const float u = 1.0f - t;
        cp->xyz[0] = second[0] * t + first[0] * u;
        cp->xyz[1] = second[1] * t + first[1] * u;
        cp->xyz[2] = t * second[2] + u * first[2];
    }
}

// ── DoRedistPatchPts (0x449A90) — "redisperse vertices" (Shift+F) ─────────────
//  Requires vertex/curve-point mode + a single selected patch.  If ANY control point is
//  picked, run the column pass over every column then the row pass over every row and
//  rebuild; otherwise print the "no vertices" message.
void DoRedistPatchPts()
{
    if ( g_qeglobals.d_select_mode != sel_vertex && g_qeglobals.d_select_mode != sel_curvepoint )
        return;

    patch_t     *patch;
    patchMesh_t *def;
    if ( !QE_SingleBrush()
         || ( patch = selected_brushes.next->patch ) == nullptr
         || ( def = patch->def ) == nullptr )
    {
        Sys_Printf( "must have a single patch selected to redistribute" );
        return;
    }

    bool any = false;
    for ( int r = 0; r < def->width; ++r )
    {
        for ( int c = 0; c < def->height; ++c )
        {
            if ( Patch_PointIsPicked( &def->ctrl[r][c] ) )
            {
                any = true;
                break;
            }
        }
    }
    if ( !any )
    {
        Sys_Printf( "No vertices selected, can't redistribute.\n" );
        return;
    }

    for ( int c = 0; c < def->height; ++c )
        Patch_RedispColumn( def, c );
    for ( int r = 0; r < def->width; ++r )
        Patch_RedispRow( def, r );

    Sys_Printf( "Redistributing patch pts.\n" );
    g_nUpdateBits = -1;
    Patch_Rebuild( def, 1 );
}

// ── DoTurnTerrainEdges (0x44B260) — "auto edge turn" (Alt+F2) ─────────────────
//  For every selected TERRAIN patch, walk each (width-1) x (height-1) quad and flip its
//  diagonal (ctrl[i][j].turned_edge bit 0) whenever the diagonal lengths differ in the
//  direction the binary tests.  The undo record is opened lazily, once per patch, on the
//  first flip; the tail stamps the undo id onto the patch's brush def + owning entity
//  (the inlined single-brush Undo_EndBrushList).  Prints the "Autofliped" tally.
//  Patch_CtrlDist (the binary's takes two drawVert_t BY VALUE) is just the xyz distance.

static float Patch_CtrlDist( const drawVert_t *a, const drawVert_t *b )   // 0x44B1B0
{
    const float dx = a->xyz[0] - b->xyz[0];
    const float dy = a->xyz[1] - b->xyz[1];
    const float dz = a->xyz[2] - b->xyz[2];
    return (float)sqrt( (double)( dx * dx + dy * dy + dz * dz ) );
}

void DoTurnTerrainEdges()
{
    int flipped = 0;
    int patches = 0;

    for ( selbrush_t *inst = selected_brushes.next; inst != &selected_brushes; inst = inst->next )
    {
        patch_t *patch = inst->patch;
        if ( !patch || patch->def->type != PATCH_TERRAIN )
            continue;

        patchMesh_t *def = patch->def;
        bool touched = false;

        for ( int i = 0; i < def->width - 1; ++i )
        {
            for ( int j = 0; j < def->height - 1; ++j )
            {
                const float d1 = Patch_CtrlDist( &def->ctrl[i + 1][j + 1], &def->ctrl[i][j] );
                const float d2 = Patch_CtrlDist( &def->ctrl[i][j + 1], &def->ctrl[i + 1][j] );
                if ( d2 == d1 )
                    continue;
                if ( ( def->ctrl[i][j].turned_edge & 1 ) != 0 )
                {
                    if ( d2 > d1 )
                        continue;
                }
                else if ( d2 < d1 )
                {
                    continue;
                }

                if ( !touched )
                {
                    Undo_ClearRedo();
                    Undo_GeneralStart( "Turn terrain edge" );
                    entity_brush_s *sym = def->pSymbiot;
                    if ( g_lastundo )
                    {
                        if ( g_lastundo->entitylist.next != &g_lastundo->entitylist )
                            Sys_Printf( "Undo_AddBrush: WARNING adding brushes after entity.\n" );
                        entity_s *owner = (entity_s *)( (brush_t *)(intptr_t)sym )->owner;
                        if ( *(int *)&owner->eclass->fixedsize )
                            Undo_AddEntity( (entity_s *)owner );
                        Undo_AddBrush( sym );
                    }
                    else
                    {
                        Sys_Printf( "Undo_AddBrush: no last undo.\n" );
                    }
                }

                def->ctrl[i][j].turned_edge ^= 1;
                ++flipped;
                touched = true;
            }
        }

        if ( touched )
        {
            ++patches;
            Patch_Rebuild( def, 1 );
        }

        // Inlined single-brush Undo_EndBrushList tail.
        entity_brush_s *sym = def->pSymbiot;
        if ( g_lastundo && !g_lastundo->done )
        {
            brush_t *bdef = (brush_t *)(intptr_t)sym;
            bdef->ownerPrev = (entity_s *)(intptr_t)g_lastundo->id;
            entity_s *owner = (entity_s *)bdef->owner;
            if ( *(int *)&owner->eclass->fixedsize )
                owner->epairEdits = g_lastundo->id;
        }
        Undo_End();
    }

    Sys_Printf( "Autofliped %i faces in %i brushes.\n", flipped, patches );
    g_nUpdateBits = -1;
}

// ── Patch_SnapVertToGrid (0x449280) — cmd: snap patch control points to the grid ─
//   Two modes: if verts are picked (d_num_move_points>0) and every selected brush is a
//   patch, snap ONLY the picked control points; otherwise snap every control point of
//   each selected patch (and Brush_SnapToGrid the non-patch brushes).
void Patch_SnapVertToGrid()
{
    selbrush_t *b = selected_brushes_next;

    // fast path: verts picked + all-patch selection → snap only the picked points.
    if ( g_qeglobals.d_num_move_points > 0 && b != &selected_brushes )
    {
        selbrush_t *scan = b;
        while ( scan->patch )
        {
            scan = scan->next;
            if ( scan == &selected_brushes )
            {
                // all selected brushes are patches.
                if ( (unsigned)g_qeglobals.d_num_move_points <= 0x401u )
                {
                    for ( int i = 0; i < g_qeglobals.d_num_move_points; ++i )
                    {
                        float *p = g_qeglobals.d_move_points[i]->xyz;
                        for ( int k = 0; k < 3; ++k )
                            p[k] = (float)floor( p[k] / grid_sizes[g_qeglobals.d_gridsize] + 0.5 )
                                 * grid_sizes[g_qeglobals.d_gridsize];
                    }
                    for ( selbrush_t *sb = selected_brushes_next; sb != &selected_brushes; sb = sb->next )
                    {
                        if ( sb->patch )
                        {
                            iassert( sb->patch->def == sb->def->patch );
                            Patch_Rebuild( sb->patch->def, 1 );
                        }
                    }
                    g_nUpdateBits = -1;
                }
                else
                    Sys_Printf( "Can only grid snap selected vertices if less than %i selected\n", 1025 );
                return;
            }
        }
    }

    // slow path: snap every control point of each selected patch, Brush_SnapToGrid the rest.
    selbrush_t *next = selected_brushes_next;
    if ( next == &selected_brushes )
        return;
    while ( 1 )
    {
        patch_t *patch = b->patch;
        if ( patch )
        {
            iassert( patch->def == b->def->patch );
            patchMesh_t *def = b->patch->def;
            for ( int col = 0; col < def->width; ++col )
                for ( int row = 0; row < def->height; ++row )
                {
                    float *p = def->ctrl[col][row].xyz;
                    for ( int k = 0; k < 3; ++k )
                        p[k] = (float)floor( p[k] / grid_sizes[g_qeglobals.d_gridsize] + 0.5 )
                             * grid_sizes[g_qeglobals.d_gridsize];
                }
            Patch_Rebuild( def, 1 );
        }
        else
            Brush_SnapToGrid( b->def );
        next = b->next;
        if ( next == &selected_brushes )
            break;
        b = b->next;
    }
}

// ── Patch_CurveToTerrain (0x43B950) — Curve→Terrain: tessellate a patch into terrain ─
//   Tessellates the patch instance `inst` (Patch_GenericMesh2), then splits the tessellated
//   grid into 16x16 (or remainder) blocks, building a terrain patch for each block that
//   copies the tessellated verts' xyz / per-layer ST / normal / colour.  Returns the last
//   created brush instance.  Reached via OnCurveToTerrain (cmd 35041).  Three per-layer
//   tessellations supply the 3 ST channels.
extern void Undo_KiwiMarkCreated( brush_t *def );   // undo.cpp (KIWI tail)
selbrush_t *Patch_CurveToTerrain( selbrush_t *inst )
{
    patchMesh_t *def = inst->patch->def;
    if ( def->curveDef )
        free( def->curveDef );
    def->curveDef = Patch_GenericMesh2( def, g_qeglobals.current_edit_layer, 0, 0 );

    // per-layer tessellations: layer 0, layer 1, and layer 2 built as layer 0 (the binary's).
    curvePatchDef_t *layerMesh[3];
    for ( int i = 0; i < 3; ++i )
        layerMesh[i] = ( i == 2 ) ? Patch_GenericMesh2( def, 0, 0, 0 )
                                  : Patch_GenericMesh2( def, i, 0, 0 );

    // KIWI: ceil (fistp 0x43B9F5); the truncating cast gave one block too few for W 17..23,
    // so the last block came out 17..23 wide - past ctrl[16][16].
    int numPatchesX = (int)KiwiCeil_Fistp( (float)( (double)( def->curveDef->width  - 1 ) / 15.0 ) );
    int numPatchesY = (int)KiwiCeil_Fistp( (float)( (double)( def->curveDef->height - 1 ) / 15.0 ) );
    iassert( numPatchesX >= 1 );
    iassert( numPatchesY >= 1 );

    selbrush_t *lastInst = 0;
    int baseX = 0;
    for ( int px = 0; px < numPatchesX; ++px )
    {
        int baseY = 0;
        int lastWidth = 16;                     // baseX advances by the column block's width-1
        for ( int py = 0; py < numPatchesY; ++py )
        {
            patchMesh_t *NewPatch = MakeNewPatch();
            NewPatch->width  = ( px >= numPatchesX - 1 ) ? ( def->curveDef->width  - baseX ) : 16;
            vassert( px < numPatchesX - 1 || ( NewPatch->width >= 2 && NewPatch->width <= 16 ),
                     "p->width %i not in [2, MAX_PATCH_WIDTH]", NewPatch->width );
            NewPatch->height = ( py >= numPatchesY - 1 ) ? ( def->curveDef->height - baseY ) : 16;
            vassert( py < numPatchesY - 1 || ( NewPatch->height >= 2 && NewPatch->height <= 16 ),
                     "p->height %i not in [2, MAX_PATCH_HEIGHT]", NewPatch->height );
            NewPatch->type = PATCH_TERRAIN;

            for ( int row = 0; row < NewPatch->height; ++row )
            {
                for ( int col = 0; col < NewPatch->width; ++col )
                {
                    int vert = baseX + col + ( baseY + row ) * def->curveDef->width;
                    curveVert_t *cv = &def->curveDef->verts[vert];
                    iassert( isfinite( cv->xyz[0] ) && isfinite( cv->xyz[1] ) && isfinite( cv->xyz[2] ) );
                    drawVert_t *cp = &NewPatch->ctrl[col][row];
                    cp->xyz[0] = cv->xyz[0];
                    cp->xyz[1] = cv->xyz[1];
                    cp->xyz[2] = cv->xyz[2];
                    for ( int L = 0; L < 3; ++L )
                    {
                        curveVert_t *lv = &layerMesh[L]->verts[vert];  // per-layer ST source
                        iassert( isfinite( lv->st[0] ) );
                        iassert( isfinite( lv->st[1] ) );
                        cp->texCoord.st[2 * L]     = lv->st[0];
                        cp->texCoord.st[2 * L + 1] = lv->st[1];
                    }
                    cp->normal[0]           = cv->normal[0];
                    cp->normal[1]           = cv->normal[1];
                    cp->normal[2]           = cv->normal[2];
                    *(int *)&cp->vert_color = *(int *)&cv->vert_color;
                }
            }

            NewPatch->texture.lyrMtl   = def->texture.lyrMtl;
            NewPatch->texture.radMtl   = def->texture.radMtl;
            NewPatch->lightmap.lyrMtl  = def->lightmap.lyrMtl;
            NewPatch->lightmap.radMtl  = def->lightmap.radMtl;
            NewPatch->smoothing.lyrMtl = def->smoothing.lyrMtl;
            NewPatch->smoothing.radMtl = def->smoothing.radMtl;
            if ( NewPatch->curveDef )
                free( NewPatch->curveDef );
            NewPatch->curveDef = Patch_GenericMesh2( NewPatch, g_qeglobals.current_edit_layer, 0, 0 );
            ++NewPatch->version;

            brush_t *nb = AddBrushForPatch( NewPatch, (entity_s *)inst->owner->def );
            Undo_KiwiMarkCreated( nb );   // KIWI: else undo left the blocks (binary links them unstamped)
            selbrush_t *newInst = (selbrush_t *)Brush_AddToList( nb, inst->owner );
            lastInst = newInst;
            if ( newInst->next || newInst->prev )
                Com_Error( ERR_FATAL, "Brush_AddToList: already linked" );
            if ( &active_brushes == &selected_brushes )
            {
                Brush_AddToList2( newInst );
            }
            else
            {
                newInst->next = active_brushes_next;
                active_brushes_next->prev = newInst;
                active_brushes_next = newInst;
                newInst->prev = &active_brushes;
            }
            baseY += NewPatch->height - 1;
            lastWidth = NewPatch->width;
        }
        baseX += lastWidth - 1;
    }

    for ( int j = 0; j < 3; ++j )
        free( layerMesh[j] );
    return lastInst;
}

// ── Patch_GetTextureName (0x448670) — the current-layer material name of the ──────
//   single selected patch (feeds Select_SetTexture_2's patch-mode name).
//   Returns the empty-string sentinel when the selection isn't a patch.
LayerMaterialDef *Patch_GetTextureName()
{
    patch_t *patch = selected_brushes_next->patch;
    if ( patch )
        return Materialdef_GetName(
                   (MaterialDef *)( &patch->def->texture + g_qeglobals.current_edit_layer ) );
    return (LayerMaterialDef *)zero;
}

// ═════════════════════════════════════════════════════════════════════════════
//  ConnectVertices (0x44A920) — Selection→Connect in curve-point mode: weld/snap
//  the selected patch control vertices.  Wired from CMainFrame::OnSelectionConnect's
//  sel_curvepoint arm.
// ═════════════════════════════════════════════════════════════════════════════

// The "reference" vertex list is a vec3[] view over three contiguous qeglobals fields:
//   unkown_pmesh_float2 (x0), unkown_pmesh_float3 (y0), then patch_verts_array02[]
//   (z0 + the rest).  Element[i] = base + 12*i as 3 floats; count = patch_verts_array02_count.
static inline float *PatchVerts02Elem( int i )
{
    return (float *)( (char *)&g_qeglobals.unkown_pmesh_float2 + 12 * i );
}

extern char g_nScaleHow;                                 // drag.cpp 0x23F16DC (axis-lock mask, low 3 bits)

// ── Patch_ConnectVerts (0x449BC0) — tolerant weld of the selected move-points to ─
//   the reference vert list (patch_verts_array02).  For each selected move point,
//   find the nearest reference vert within tolerant_weld² and snap the move point to it.
static void Patch_ConnectVerts()
{
    if ( g_qeglobals.d_num_move_points < 1 )
    {
        Sys_Printf( "can only connect patch vertices if at least 1 vertices are selected\n" );
        return;
    }
    if ( (unsigned)g_qeglobals.d_num_move_points > 0x1001u )
    {
        Sys_Printf( "can only connect patch vertices if at most %i vertices are selected\n", 4097 );
        return;
    }

    // The binary also fills a scratch save buffer it never reads; the observable effect is
    // only the snap, so the buffer is omitted.
    float tol   = (float)( g_PrefsDlg->tolerant_weld * g_PrefsDlg->tolerant_weld );
    int   merged = 0;
    if ( g_qeglobals.d_num_move_points > 0 )
    {
        for ( int i = 0; i < g_qeglobals.d_num_move_points; ++i )
        {
            float bestDistSq = tol + 1.0f;   // nothing found yet
            int   best       = -1;
            for ( int j = 0; j < g_qeglobals.patch_verts_array02_count; ++j )
            {
                drawVert_t *mp = g_qeglobals.d_move_points[i];
                float *ref = PatchVerts02Elem( j );
                float dx = ref[0] - mp->xyz[0];
                float dy = ref[1] - mp->xyz[1];
                float d2 = dy * dy + dx * dx;
                if ( d2 <= tol && bestDistSq > (double)d2 )
                {
                    bestDistSq = d2;
                    best       = j;
                }
            }
            if ( bestDistSq <= tol )
            {
                drawVert_t *mp = g_qeglobals.d_move_points[i];
                ++merged;
                float *ref = PatchVerts02Elem( best );
                mp->xyz[0] = ref[0];
                mp->xyz[1] = ref[1];
                mp->xyz[2] = ref[2];
            }
        }
    }
    if ( merged )
    {
        if ( merged == 1 )
            Sys_Printf( "Merged 1 vertex\n" );
        else
            Sys_Printf( "Merged %i vertices\n", merged );
    }
    else
    {
        Sys_Printf( "No vertices are close enough to do Tolerant Weld\n" );
    }
    g_qeglobals.d_num_move_points = 0;
    for ( selbrush_t *b = selected_brushes_next; b != &selected_brushes; b = b->next )
        if ( b->patch )
            Patch_Rebuild( b->patch->def, 1 );
    g_nUpdateBits = -1;
}

// ── Patch_ConnectVerts_02 (0x449D90) — snap the selected move-points to reference ─
//   element[0] (patch_verts_array02[0]), per the g_nScaleHow axis-lock mask (a bit
//   SET locks that axis; a clear bit snaps it).  Resets both point counts on exit.
static void Patch_ConnectVerts_02()
{
    if ( g_qeglobals.d_num_move_points < 1 )
    {
        Sys_Printf( "can only connect patch vertices if at least 1 patch vertices are selected\n" );
        return;
    }
    if ( (unsigned)g_qeglobals.d_num_move_points > 0x401u )
    {
        Sys_Printf( "can only connect patch vertices if at most %i vertices are selected\n", 1025 );
        return;
    }
    if ( g_qeglobals.d_num_move_points > 0 )
    {
        int lockX = g_nScaleHow & 1;
        int lockY = g_nScaleHow & 2;
        int lockZ = g_nScaleHow & 4;
        float *ref = PatchVerts02Elem( 0 );          // {unkown_pmesh_float2, _float3, patch_verts_array02[0]}
        for ( int i = 0; i < g_qeglobals.d_num_move_points; ++i )
        {
            drawVert_t *mp = g_qeglobals.d_move_points[i];
            if ( lockX || lockY || lockZ )
            {
                if ( !lockX ) mp->xyz[0] = ref[0];
                if ( !lockY ) mp->xyz[1] = ref[1];
                if ( !lockZ ) mp->xyz[2] = ref[2];
            }
            else
            {
                mp->xyz[0] = ref[0];
                mp->xyz[1] = ref[1];
                mp->xyz[2] = ref[2];
            }
        }
    }
    g_qeglobals.d_num_move_points        = 0;
    g_qeglobals.patch_verts_array02_count = 0;
    for ( selbrush_t *b = selected_brushes_next; b != &selected_brushes; b = b->next )
        if ( b->patch )
            Patch_Rebuild( b->patch->def, 1 );
    g_nUpdateBits = -1;
}

// ── Patch_ConnectVerts_03 (0x449EF0) — tolerant weld AMONG the selected move-points ─
//   (no reference list): every pair (outer i > inner j) within tolerant_weld² is welded
//   by moving the outer point onto the inner.  Requires an all-patch, no-face selection.
//   The binary's ~48KB write-only undo snapshot buffer is never read back, so it is
//   omitted.
static void Patch_ConnectVerts_03()
{
    if ( g_SelectedFaces.GetSize() > 0 )
    {
        Sys_Printf( "can only connect patch vertices if only patches are selected\n" );
        return;
    }
    selbrush_t *b = selected_brushes_next;
    if ( b == &selected_brushes )
    {
        Sys_Printf( "can only connect patch vertices if only patches are selected\n" );
        return;
    }
    while ( b->patch )
    {
        b = b->next;
        if ( b == &selected_brushes )
        {
            // whole selection is patches → do the weld
            if ( g_qeglobals.d_num_move_points < 2 )
            {
                Sys_Printf( "can only connect patch vertices if at least 2 vertices are selected\n" );
                return;
            }
            if ( (unsigned)g_qeglobals.d_num_move_points > 0x1001u )
            {
                Sys_Printf( "can only connect patch vertices if at most %i vertices are selected\n", 4097 );
                return;
            }
            float tol    = (float)( g_PrefsDlg->tolerant_weld * g_PrefsDlg->tolerant_weld );
            int   merged = 0;
            // outer index i runs high→low over d_move_points; each is welded against every
            // lower-index point j.
            for ( int i = g_qeglobals.d_num_move_points - 1; i > 0; --i )
            {
                float *outer = g_qeglobals.d_move_points[i]->xyz;
                for ( int j = i - 1; j >= 0; --j )
                {
                    float *inner = g_qeglobals.d_move_points[j]->xyz;
                    float dx = inner[0] - outer[0];
                    float dy = inner[1] - outer[1];
                    float d2 = dy * dy + dx * dx;
                    if ( d2 <= tol )
                    {
                        ++merged;
                        // outer ← inner
                        outer[0] = inner[0];
                        outer[1] = inner[1];
                        outer[2] = inner[2];
                    }
                }
            }
            if ( merged )
            {
                if ( merged == 1 )
                    Sys_Printf( "Merged 1 vertex\n" );
                else
                    Sys_Printf( "Merged %i vertices\n", merged );
            }
            else
            {
                Sys_Printf( "No vertices are close enough to do Tolerant Weld\n" );
            }
            g_qeglobals.d_num_move_points = 0;
            for ( selbrush_t *sb = selected_brushes_next; sb != &selected_brushes; sb = sb->next )
                if ( sb->patch )
                    Patch_Rebuild( sb->patch->def, 1 );
            g_nUpdateBits = -1;
            return;
        }
    }
    // a non-patch was in the selection → the "only patches" contract failed
    Sys_Printf( "can only connect patch vertices if only patches are selected\n" );
}

// ── ConnectVertices (0x44A920) — dispatch on the TolerantWeld pref ────────────────
void ConnectVertices()
{
    if ( g_PrefsDlg->m_bTolerantWeld != 1 )
    {
        // manual axis-snap path: require a patch in the selection.
        selbrush_t *b = selected_brushes_next;
        if ( b == &selected_brushes )
        {
            Sys_Printf( "can only connect patch vertices if a patch selected\n" );
            return;
        }
        while ( !b->patch )
        {
            b = b->next;
            if ( b == &selected_brushes )
            {
                Sys_Printf( "can only connect patch vertices if a patch selected\n" );
                return;
            }
        }
        if ( g_qeglobals.patch_verts_array02_count > 0 )
        {
            Patch_ConnectVerts_02();
            return;
        }
        if ( g_qeglobals.d_num_move_points < 2 )
        {
            Sys_Printf( "can only connect patch vertices if at least 2 vertices are selected\n" );
            return;
        }
        if ( (unsigned)g_qeglobals.d_num_move_points > 0x401u )
        {
            Sys_Printf( "can only connect patch vertices if at most %i vertices are selected\n", 1025 );
            return;
        }
        // snap each selected move point (1..N-1) to move_points[0] on the axes NOT locked
        // in g_nScaleHow (a set bit locks that axis; g_nScaleHow==0 snaps all three).
        drawVert_t *pt0 = g_qeglobals.d_move_points[0];
        for ( int i = 1; i < g_qeglobals.d_num_move_points; ++i )
        {
            drawVert_t *mp = g_qeglobals.d_move_points[i];
            if ( !( g_nScaleHow & 1 ) ) mp->xyz[0] = pt0->xyz[0];
            if ( !( g_nScaleHow & 2 ) ) mp->xyz[1] = pt0->xyz[1];
            if ( !( g_nScaleHow & 4 ) ) mp->xyz[2] = pt0->xyz[2];
        }
        g_qeglobals.d_num_move_points = 0;
        for ( selbrush_t *sb = selected_brushes_next; sb != &selected_brushes; sb = sb->next )
            if ( sb->patch )
                Patch_Rebuild( sb->patch->def, 1 );
        g_nUpdateBits = -1;
        return;
    }

    // TolerantWeld pref set: all-patch + no-face selection → weld among the move points
    // (Patch_ConnectVerts_03); otherwise weld to the reference list (Patch_ConnectVerts).
    if ( g_SelectedFaces.GetSize() <= 0 )
    {
        selbrush_t *b = selected_brushes_next;
        if ( b != &selected_brushes )
        {
            while ( b->patch )
            {
                b = b->next;
                if ( b == &selected_brushes )
                {
                    Patch_ConnectVerts_03();
                    return;
                }
            }
        }
    }
    Patch_ConnectVerts();
}

// ═════════════════════════════════════════════════════════════════════════════
//  FILLED / TEXTURED PATCH SURFACE - the camera/world patch draw.  3D camera world
//  patches (DrawBrush 0x47b070, draw_meth2 != 29) route to DrawPatches, whose DEFAULT arm
//  (patch_wireframe OFF) draws the patch FILLED per-layer through the same surf cache the
//  models use:
//    DrawPatches -> Patch_Fill_Emit -> Editor_AddMeshCmd (r_ed_scene.cpp)
//  gated by Patch_Fill_SyncVersion -> Patch_BuildInstanceVisuals,
//  which builds the patch INSTANCE per-layer visuals (the patch analogue of the brush
//  Visuals_InitFaceVis / Face_BuildLayerGeom -> Editor_VB_Upload flow).
//  Patch_Fill_BuildVisuals computes per-vertex TANGENT/BINORMAL (TangentSpace_Calc,
//  universal/tangentspace.cpp) before Editor_VB_Upload; those vectors are render-visible
//  for normal/specular techniques, so they are computed rather than uploaded as zeroes.
// ═════════════════════════════════════════════════════════════════════════════

// ── deps (all ported elsewhere) ──────────────────────────────────────────────
extern int          MaterialDef_11( MaterialDef *m );                              // materialdef.cpp — layer count
extern Material    *MaterialDef_14( unsigned int visIndex, MaterialDef *m );        // materialdef.cpp 0x431C60
extern void         PerpendicularVector( const float *src, float *dst );             // 0x4A5340
extern char         Material_GetConstantValue( Material *mtl, const char *name, float *out ); // materialdef 0x51AD50
extern unsigned int Editor_VB_Upload( Material *material, int vertCount,
                        const float *xyz, const float *tangent, const float *binormal,
                        const float *normal, const float *texCoord, const float *color ); // r_ed_vertbuf.cpp 0x51D1D0
extern char         R_Ed_FreeVertices( Material *handle, int vertCount, int vertHandle );  // r_ed_vertbuf.cpp 0x51CB70
extern int          Editor_MaterialSortKey( Material *handle );                            // r_ed_scene.cpp 0x4FDBB0
extern void         Editor_AddMeshCmd( Material *handle, int techType, int sortKey,
                        int vertCount, int vbIndexAndOffs, int indexCount, const uint16_t *indexTable ); // r_ed_scene.cpp 0x4FDA50
extern const MaterialTechnique *__cdecl Material_GetTechnique( const Material *material,
                        MaterialTechniqueType techType );

// PM_FRONT_FACE / PM_BACK_FACE index selectors come from qe3.h.

static float Patch_TangentDot( const float *a, const float *b )
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static float Patch_TangentAngle( const float *a, const float *b )
{
    const float dot = Patch_TangentDot( a, b );
    if ( dot <= -1.0f )
        return -(float)KIWI_PI;   // KIWI: KIWI_PI
    if ( dot >= 1.0f )
        return (float)KIWI_PI;
    return (float)acos( dot );
}

// Patch_TangentBasisForTri (0x4BAC40): derive one triangle's tangent/binormal candidates from xyz+st.
static void Patch_TangentBasisForTri( const float *st0, const float *xyz0,
                                      const float *st1, const float *xyz1,
                                      const float *st2, const float *xyz2,
                                      float *outTangent, float *outBinormal )
{
    const float du1 = st1[0] - st0[0];
    const float du2 = st2[0] - st0[0];
    const float dv1 = st1[1] - st0[1];
    const float dv2 = st2[1] - st0[1];
    const float e1[3] = { xyz1[0] - xyz0[0], xyz1[1] - xyz0[1], xyz1[2] - xyz0[2] };
    const float e2[3] = { xyz2[0] - xyz0[0], xyz2[1] - xyz0[1], xyz2[2] - xyz0[2] };

    if ( dv2 * du1 >= dv1 * du2 )
    {
        outTangent[0] = e1[0] * dv2 - e2[0] * dv1;
        outTangent[1] = e1[1] * dv2 - e2[1] * dv1;
        outTangent[2] = e1[2] * dv2 - e2[2] * dv1;
        outBinormal[0] = e2[0] * du1 - e1[0] * du2;
        outBinormal[1] = e2[1] * du1 - e1[1] * du2;
        outBinormal[2] = e2[2] * du1 - e1[2] * du2;
    }
    else
    {
        outTangent[0] = e2[0] * dv1 - e1[0] * dv2;
        outTangent[1] = e2[1] * dv1 - e1[1] * dv2;
        outTangent[2] = e2[2] * dv1 - e1[2] * dv2;
        outBinormal[0] = e1[0] * du2 - e2[0] * du1;
        outBinormal[1] = e1[1] * du2 - e2[1] * du1;
        outBinormal[2] = e1[2] * du2 - e2[2] * du1;
    }

    Vec3Normalize_R( outTangent );
    Vec3Normalize_R( outBinormal );
}

// Patch_TangentTriWeights (0x4BAE90): angle weights for a triangle's three vertices.
static void Patch_TangentTriWeights( const float *xyz, int i0, int i1, int i2, float *outWeights )
{
    const float *p0 = xyz + 3 * i0;
    const float *p1 = xyz + 3 * i1;
    const float *p2 = xyz + 3 * i2;
    float v01[3] = { p0[0] - p1[0], p0[1] - p1[1], p0[2] - p1[2] };
    float v12[3] = { p1[0] - p2[0], p1[1] - p2[1], p1[2] - p2[2] };
    float v20[3] = { p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };

    Vec3Normalize_R( v01 );
    Vec3Normalize_R( v12 );
    Vec3Normalize_R( v20 );
    outWeights[0] = Patch_TangentAngle( v01, v20 );
    outWeights[1] = Patch_TangentAngle( v12, v01 );
    outWeights[2] = Patch_TangentAngle( v20, v12 );
}

// TangentSpace_Calc 0x4BAF60, specialized to Patch_Fill_BuildVisuals's contiguous scratch arrays.
static void Patch_TangentSpaceCalc( int vertCount, const uint16_t *indices, int indexCount,
                                    float *xyz, float *normal, float *st,
                                    float *tangent, float *binormal )
{
    for ( int i = 0; i < vertCount; ++i )
    {
        tangent[3*i+0] = tangent[3*i+1] = tangent[3*i+2] = 0.0f;
        binormal[3*i+0] = binormal[3*i+1] = binormal[3*i+2] = 0.0f;
    }

    for ( int tri = 0; tri + 2 < indexCount; tri += 3 )
    {
        const int i0 = indices[tri + 0];
        const int i1 = indices[tri + 1];
        const int i2 = indices[tri + 2];
        iassert( i0 >= 0 && i0 < vertCount );
        iassert( i1 >= 0 && i1 < vertCount );
        iassert( i2 >= 0 && i2 < vertCount );

        float triTan[3], triBin[3], weights[3];
        Patch_TangentBasisForTri( st + 2*i0, xyz + 3*i0,
                                  st + 2*i1, xyz + 3*i1,
                                  st + 2*i2, xyz + 3*i2,
                                  triTan, triBin );
        Patch_TangentTriWeights( xyz, i0, i1, i2, weights );

        const int idx[3] = { i0, i1, i2 };
        for ( int corner = 0; corner < 3; ++corner )
        {
            float *dstT = tangent + 3 * idx[corner];
            float *dstB = binormal + 3 * idx[corner];
            const float w = weights[corner];
            dstT[0] += triTan[0] * w; dstT[1] += triTan[1] * w; dstT[2] += triTan[2] * w;
            dstB[0] += triBin[0] * w; dstB[1] += triBin[1] * w; dstB[2] += triBin[2] * w;
        }
    }

    for ( int i = 0; i < vertCount; ++i )
    {
        float *n = normal + 3 * i;
        float *t = tangent + 3 * i;
        float *b = binormal + 3 * i;
        const float ndott = Patch_TangentDot( n, t );
        t[0] -= n[0] * ndott;
        t[1] -= n[1] * ndott;
        t[2] -= n[2] * ndott;
        if ( Vec3Normalize_R( t ) < 0.001f )
        {
            Vec3Cross( b, n, t );
            if ( Vec3Normalize_R( t ) < 0.001f )
                PerpendicularVector( n, t );
        }

        float cross[3];
        Vec3Cross( n, t, cross );
        const float signDot = Patch_TangentDot( b, cross );
        if ( signDot >= 0.0f )
        {
            b[0] = cross[0]; b[1] = cross[1]; b[2] = cross[2];
        }
        else
        {
            b[0] = -cross[0]; b[1] = -cross[1]; b[2] = -cross[2];
        }
    }
}

// ── Patch_Fill_BuildFrontIndices (0x43FB70) — build the FRONT-face triangle indices for the tessellated grid ──
// Two triangles per quad cell over the (width-1)x(height-1) grid; for a TERRAIN patch
// (type==64) the quad's split diagonal honours ctrl[col][row].turned_edge&1.  Returns the
// index count written.  (def = the patchMesh_t DEF; out = uint16_t* index buffer.)
static int Patch_Fill_BuildFrontIndices( patchMesh_t *def, uint16_t *out )
{
    curvePatchDef_t *mesh = def->curveDef;
    if ( !mesh ) return 0;
    const int mw = mesh->width;
    const int mh = mesh->height;
    const bool terrain = ( (int)def->type == 64 );
    int idx = 0;                         // running index-write count
    // rows: 0..mh-2 ; cols: 0..mw-2
    for ( int row = 0; row + 1 < mh; ++row )
    {
        for ( int col = 0; col + 1 < mw; ++col )
        {
            const int base = row * mw + col;
            const int v00 = base;                         // (row,   col)
            const int v01 = base + 1;                     // (row,   col+1)
            const int v10 = base + mw;                    // (row+1, col)
            const int v11 = base + mw + 1;                // (row+1, col+1)
            // the turned flag is ctrl[col][row].
            const bool turned = terrain && ( def->ctrl[col][row].turned_edge & 1 );
            if ( turned )
            {
                // {v00, v11, v01} , {v11, v00, v10}
                out[idx+0] = (uint16_t)v00; out[idx+1] = (uint16_t)v11; out[idx+2] = (uint16_t)v01;
                out[idx+3] = (uint16_t)v11; out[idx+4] = (uint16_t)v00; out[idx+5] = (uint16_t)v10;
            }
            else
            {
                // {v00, v10, v01} , {v01, v10, v11}
                out[idx+0] = (uint16_t)v00; out[idx+1] = (uint16_t)v10; out[idx+2] = (uint16_t)v01;
                out[idx+3] = (uint16_t)v01; out[idx+4] = (uint16_t)v10; out[idx+5] = (uint16_t)v11;
            }
            idx += 6;
        }
    }
    return idx;
}

// ── Patch_Fill_BuildIndices (0x4401a0) — allocate + fill the front indices, then the back (reverse) ──
static void Patch_Fill_BuildIndices( patch_t *inst )
{
    curvePatchDef_t *mesh = inst->def->curveDef;
    const int mw = mesh->width, mh = mesh->height;
    const int indexCount = ( mh - 1 ) * ( 6 * mw - 6 );
    inst->indexCount = indexCount;
    if ( indexCount <= 0 ) { inst->indicesFront = inst->indicesBack = nullptr; return; }
    // ONE alloc for both faces: front[indexCount] then back[indexCount].
    uint16_t *buf = (uint16_t *)operator new( (size_t)( 4 * indexCount ) );  // 2*indexCount uint16 each face
    inst->indicesFront = buf;
    inst->indicesBack  = buf + indexCount;
    int written = Patch_Fill_BuildFrontIndices( inst->def, buf );
    iassert( written == indexCount );                    // "indexCount == pm->indexCount"
    // back face = front reversed (back[i] = front[indexCount-i-1]).
    for ( int i = 0; i < indexCount; ++i )
        inst->indicesBack[i] = inst->indicesFront[indexCount - i - 1];
}

// ── Patch_Fill_BuildVisuals (0x43fd40) — build per-vertex world geometry + upload per layer ──
static void Patch_Fill_BuildVisuals( patch_t *inst, const orientation_t *orient, MaterialDef *mtldef )
{
    curvePatchDef_t *mesh = inst->def->curveDef;
    const int vertCount = mesh->width * mesh->height;
    inst->vertCount = vertCount;
    curveVert_t *src = mesh->verts;

    // Scratch per-vertex arrays (freed at the end).
    float *xyz    = (float *)operator new( (size_t)( 12 * vertCount ) );  // 3 floats
    float *normal = (float *)operator new( (size_t)( 12 * vertCount ) );
    float *st     = (float *)operator new( (size_t)( 8  * vertCount ) );  // 2 floats
    float *tangent  = (float *)operator new( (size_t)( 12 * vertCount ) );
    float *binormal = (float *)operator new( (size_t)( 12 * vertCount ) );
    unsigned int *color = (unsigned int *)operator new( (size_t)( 4 * vertCount ) );

    // colorTint (material constant "colorTint"; default white).
    Material *m0 = MaterialDef_14( 0, mtldef );
    iassert( m0 );                                       // MaterialDef.cpp:336
    float tint[4];
    if ( !Material_GetConstantValue( m0, "colorTint", tint ) )
        tint[0] = tint[1] = tint[2] = tint[3] = 1.0f;
    GfxColor tintC;
    Byte4PackPixelColor( tint, &tintC );

    // Lightmap-alpha mode flag: (layeredMaterial->unk_flags2 & 0x70) == 96.
    qtexture_s *lm = MaterialDef_GetLayeredMaterial( mtldef );
    const bool lightmapAlpha = lm && ( ( lm->unk_flags2 & 0x70 ) == 96 );

    for ( int i = 0; i < vertCount; ++i )
    {
        curveVert_t *cv = &src[i];
        // position (world) + normal (world); degenerate normal -> +Z.
        OrientationPosToWorldPos( &xyz[3*i], cv->xyz, orient );
        OrientationDirToWorldDir( &normal[3*i], orient, cv->normal );
        if ( Vec3Normalize_R( &normal[3*i] ) < 0.001f )
        { normal[3*i+0] = 0.0f; normal[3*i+1] = 0.0f; normal[3*i+2] = 1.0f; }
        // texcoords (curveVert.st).
        st[2*i+0] = cv->st[0];
        st[2*i+1] = cv->st[1];
        // per-vertex colour, BGRA byte order (Byte4PackPixelColor: array[0]=B, [1]=G, [2]=R, [3]=A).
        // alpha byte = curveVert.vert_color.a.
        const unsigned a = ( (const byte *)&cv->vert_color )[3];
        byte B, G, R, A;
        if ( !lightmapAlpha )
        {
            // colour bytes = colorTint verbatim; alpha byte from the vert.
            B = tintC.array[0]; G = tintC.array[1]; R = tintC.array[2]; A = (byte)a;
        }
        else
        {
            // lightmap-alpha mode multiplies RGB by vertex alpha; final alpha byte = 0xFF.
            auto mul255 = []( unsigned c, unsigned aa ) -> byte {
                unsigned t = c * aa + 127;               // +127 rounding
                return (byte)( t / 255 );
            };
            B = mul255( tintC.array[0], a );
            G = mul255( tintC.array[1], a );
            R = mul255( tintC.array[2], a );
            A = 0xFF;
        }
        color[i] = (unsigned)B | ( (unsigned)G << 8 ) | ( (unsigned)R << 16 ) | ( (unsigned)A << 24 );
    }

    iassert( inst->indicesFront );                       // PMESH.CPP:3670
    Patch_TangentSpaceCalc( vertCount, inst->indicesFront, inst->indexCount,
                            xyz, normal, st, tangent, binormal );

    // Upload one VB run per layer + record {material, vertHandle}.
    // Runs past the material def's own layer count are KIWI texture layers: their
    // material is the layer's alpha-blend twin and their vertex alpha is the weight
    // channel (kiwi_terrain.cpp rewrites `color` per run).
    const int baseRuns = MaterialDef_11( mtldef );
    for ( int L = 0; L < inst->visCount; ++L )
    {
        Material *lmat = MaterialDef_14( (unsigned)( L < baseRuns ? L : 0 ), mtldef );
        if ( L < baseRuns )
            KiwiMatConvert_ApplyPatchLightmapDiagnostic( inst->def, color, vertCount, &lmat ); // KIWI-UX: non-lit base material replaces the checker with red
        {
            extern void KiwiTerrain_LayerUpload( patchMesh_t *def, int run, int baseRuns,
                                                 unsigned int *color, const curveVert_t *verts,
                                                 int vertCount, Material **material );
            KiwiTerrain_LayerUpload( inst->def, L, baseRuns, color, src, vertCount, &lmat );
        }
        inst->visArray[L].material   = lmat;
        inst->visArray[L].vertHandle = (int)Editor_VB_Upload( lmat, vertCount,
            xyz, tangent, binormal, normal, st, (const float *)color );
    }

    operator delete( color );
    operator delete( binormal );
    operator delete( tangent );
    operator delete( st );
    operator delete( normal );
    operator delete( xyz );
}

// ── Patch_Fill_FreeVisuals (0x43f8c0) — FREE the current instance visuals (before rebuild) ──
static void Patch_Fill_FreeVisuals( patch_t *inst )
{
    iassert( inst );
    if ( inst->visArray )
    {
        for ( int i = 0; i < inst->visCount; ++i )
            R_Ed_FreeVertices( inst->visArray[i].material, inst->vertCount, inst->visArray[i].vertHandle );
        operator delete( inst->visArray );
        // indicesBack == indicesFront + indexCount (ONE allocation, back half in the same
        // block); the binary checks it as a byte offset of 2*indexCount (uint16 indices).
        iassert( inst->indicesBack == inst->indicesFront + inst->indexCount );
        inst->visArray = nullptr;
        inst->visCount = 0;
        if ( inst->indicesFront )
            operator delete( inst->indicesFront );       // one alloc (front, back = front+indexCount)
        inst->indicesFront = nullptr;
        inst->indicesBack  = nullptr;
        inst->indexCount   = 0;
        inst->vertCount    = 0;
    }
    else
    {
        iassert( !inst->visCount );
        iassert( !inst->indicesFront );
        iassert( !inst->indicesBack );
        iassert( !inst->indexCount );
        iassert( !inst->vertCount );
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  PATCH DEALLOC CHAIN - the three patch free paths.  Skipping any of them leaks the
//  mesh/instance/index buffers and leaves symbiot->patch dangling at a freed patchMesh_t.
// ═══════════════════════════════════════════════════════════════════════════════

// ── Patch_Fill_ResetVisuals (0x43FA20) — free the instance's index/vis buffers and ARM the
//  rebuild (version = def->version - 1).  DELIBERATE TWIN of Patch_Fill_FreeVisuals
//  above: this one has NO R_Ed_FreeVertices loop — it does not hand vertices back to the
//  renderer — and, unlike Patch_Fill_FreeVisuals, it ends by resetting the instance version.  Do NOT
//  "unify" these two; the asymmetry is the binary's.
void Patch_Fill_ResetVisuals( patch_t *pm )
{
    iassert( pm );   // pmesh.cpp:3520
    if ( pm->visArray )
    {
        free( pm->visArray );                               // (no R_Ed_FreeVertices loop)
        bool contiguous = ( pm->indicesBack == pm->indicesFront + pm->indexCount );
        pm->visArray  = nullptr;
        pm->visCount  = 0;
        if ( !contiguous ) {}                               // (evaluated pre-clear above)
        iassert( pm->indices[PM_BACK_FACE] == pm->indices[PM_FRONT_FACE] + pm->indexCount );   // PMESH.CPP:3536
        free( pm->indicesFront );                           // (one alloc: back = front + count)
        pm->indicesFront = nullptr;
        pm->indicesBack  = nullptr;
        pm->indexCount   = 0;
        pm->vertCount    = 0;
        pm->version = (short)( pm->def->version - 1 );      // arm the rebuild
    }
    else
    {
        iassert( !pm->visCount );   // pmesh.cpp:3524
        iassert( !pm->indices[PM_FRONT_FACE] );   // PMESH.CPP:3525
        iassert( !pm->indices[PM_BACK_FACE] );    // PMESH.CPP:3526
        iassert( !pm->indexCount );   // pmesh.cpp:3527
        iassert( !pm->vertCount );   // pmesh.cpp:3528
    }
}

// ── Patch_FreeInstance (0x442760) — free a patch INSTANCE: release its visuals through
//  Patch_Fill_FreeVisuals (the R_Ed_FreeVertices variant — this path DOES return the GPU
//  vertices), free the instance, refresh the Patch Inspector.
void Patch_FreeInstance( patch_t *p )
{
    iassert( p );   // pmesh.cpp:4549
    iassert( p->def );   // pmesh.cpp:4550
    Patch_Fill_FreeVisuals( p );
    free( p );
    if ( PatchDialog_IsOpen() )
        g_PatchDialog_GetPatchInfo();
}

// ── Patch_Delete (0x4426A0) — free a patch DEF (patchMesh_t):
//  break the symbiot back-pointer (brush_t.patch) so it can't dangle, free the
//  tessellated curveDef, free the def, refresh the Patch Inspector.  The three asserts
//  are the binary's (symbiot present / back-pointer identity / refCount drained).
void Patch_Delete( patchMesh_t *patchDef )
{
    patchMesh_t *p = (patchMesh_t *)(intptr_t)patchDef;
    brush_t *symbiot = p->symbiot;
    iassert( p->symbiot );                // PMESH.CPP:4534
    iassert( p->symbiot->patch == p );    // PMESH.CPP:4535
    vassert( (p->symbiot->refCount == 0), "(p->symbiot->refCount) = %i", p->symbiot->refCount );   // PMESH.CPP:4536
    if ( symbiot )
        symbiot->patch = nullptr;                           // kill the dangling back-ref
    if ( p->curveDef )
        free( p->curveDef );
    free( p );
    if ( PatchDialog_IsOpen() )
        g_PatchDialog_GetPatchInfo();
}

// ── Patch_BuildInstanceVisuals (0x440240) — free-then-rebuild the instance visuals ──
// `orient` is the PLACEMENT orientation (prefab-content patches ride the composed prefabOrient;
// top-level patches ride world_orient_matrix).  It is threaded DrawPatches->Patch_Fill_SyncVersion->
// here->Patch_Fill_BuildVisuals so each vertex is world-transformed (OrientationPosToWorldPos); hardcoding
// world_orient_matrix here builds every filled patch at its LOCAL coords.
static void Patch_BuildInstanceVisuals( patch_t *inst, const orientation_t *orient )
{
    Patch_Fill_FreeVisuals( inst );
    // Current-edit-layer material def (&texture[layer]).
    MaterialDef *mtldef = (MaterialDef *)( &inst->def->texture + g_qeglobals.current_edit_layer );
    // Prefab-content patches load with a DEGENERATE material (headless/prefab shim, layerCount 0);
    // realize it by name now that the renderer is up, else the filled draw has nothing to bind.
    extern bool Materialdef_Realize( MaterialDef *md );  // materialdef.cpp
    Materialdef_Realize( mtldef );
    // KIWI texture layers: one extra blended run per used slot (preview only; the .map
    // keeps one patch).  Only the base material def carries them (edit layer 0).
    extern int KiwiTerrain_ExtraLayerCount( patchMesh_t *def );   // kiwi_terrain.cpp
    const int baseLayers = MaterialDef_11( mtldef );
    const int layerCount = baseLayers
                         + ( g_qeglobals.current_edit_layer == 0 ? KiwiTerrain_ExtraLayerCount( inst->def ) : 0 );
    inst->visCount = layerCount;
    if ( layerCount <= 0 ) { inst->visArray = nullptr; return; }
    inst->visArray = (patchVisuals_s *)operator new( (size_t)layerCount * sizeof(patchVisuals_s) );
    if ( !inst->visArray )
        Error( "Out of memory on patch visuals array" );
    Patch_Fill_BuildIndices( inst );
    Patch_Fill_BuildVisuals( inst, orient, mtldef );     // world-transform each vertex by `orient`
    // sync the instance version to the def.
    inst->version = inst->def->version;
}

// ── Patch_Fill_SyncVersion (0x4405a0) — rebuild only when the def version advanced ──
// `orient` = the PLACEMENT orientation (threaded from DrawPatches).  Used only on rebuild;
// version-gated exactly like the brush faceVis (Brush_CheckBuildFaceVis) — a per-placement patch instance
// builds once with its own placement orient, then the cache holds.
// NOT static: the sun-preview PATCH shadow arm (Patch_AddShadowSilhouette 0x4418B0,
// shadowvolume.cpp) drives it too, as the binary does.
void Patch_Fill_SyncVersion( patch_t *inst, const orientation_t *orient )
{
    // Rebuild if no visuals yet OR the instance version != def version.
    if ( !inst->visArray || inst->version != inst->def->version )
        Patch_BuildInstanceVisuals( inst, orient );
}

// ═════════════════════════════════════════════════════════════════════════════
//  KIWI-UX: CROSS-PATCH NORMAL WELD — kill the lighting seam between adjacent patches.
//
//  Each patch tessellates its curveDef normals from its OWN control mesh, so two sewn
//  terrain patches disagree about the shared boundary's normal and the editor preview
//  (world N.L shading and the sun re-add both read the tessellated normal,
//  Patch_Fill_BuildVisuals :9959) shows a hard lighting seam the COMPILED map does not
//  have: cod4map averages normals across coincident vertices at BSP time
//  (SmoothVertexNormalsForGroup, cod4map/tris.cpp:2606, default smoothAngle 0.01 ≈
//  anything under ~89 degrees apart smooths).  This pass mirrors that for the preview:
//  gather the PERIMETER vertices of every top-level patch's tessellated grid, group
//  coincident positions, average each member's normal with every co-located normal
//  within the compiler's dot >= 0.01 gate, write the result back into curveDef, and
//  bump the def version so the lazy instance rebuild re-uploads.
//
//  Scope notes: def-level curveDef coords are LOCAL, so only top-level patches
//  (world-identity placement) are welded — prefab-content patch pairs in different
//  prefabs live in different spaces and are skipped.  T-junctions (different
//  tessellation densities) simply find no partner and keep their own normal.
//  Change detection is a stamp over (def, version) so an idle map costs one list walk.
// ═════════════════════════════════════════════════════════════════════════════
#include <vector>
#include <algorithm>

namespace
{
    struct kpwVert_t
    {
        unsigned long long key;      // quantized position
        patchMesh_t       *def;
        curveVert_t       *cv;
        float              newNormal[3];
        bool               changed;
    };

    // Quantize to 1/4 unit; coincident (sewn) tessellated verts land on identical keys.
    unsigned long long KPW_Key( const float *p )
    {
        const long long qx = (long long)floorf( p[0] * 4.0f + 0.5f );
        const long long qy = (long long)floorf( p[1] * 4.0f + 0.5f );
        const long long qz = (long long)floorf( p[2] * 4.0f + 0.5f );
        return ( (unsigned long long)( qx & 0x1FFFFF ) )
             | ( (unsigned long long)( qy & 0x1FFFFF ) << 21 )
             | ( (unsigned long long)( qz & 0x1FFFFF ) << 42 );
    }

    void KPW_CollectPerimeter( patchMesh_t *def, std::vector<kpwVert_t> &out )
    {
        curvePatchDef_t *mesh = def->curveDef;
        const int mw = mesh->width, mh = mesh->height;
        for ( int row = 0; row < mh; ++row )
            for ( int col = 0; col < mw; ++col )
            {
                if ( row != 0 && row != mh - 1 && col != 0 && col != mw - 1 )
                    continue;                              // perimeter only
                curveVert_t *cv = &mesh->verts[col + row * mw];
                kpwVert_t v;
                v.key = KPW_Key( cv->xyz );
                v.def = def;
                v.cv  = cv;
                v.changed = false;
                out.push_back( v );
            }
    }
}

void KiwiPatchWeld_Run()
{
    // ── change stamp over every top-level patch (def pointer + version) ──────
    static unsigned long long s_lastStamp = 0xFFFFFFFFFFFFFFFFull;
    unsigned long long stamp = 0x9E3779B97F4A7C15ull;
    bool anyMissingMesh = false;
    selbrush_t *lists[2] = { &active_brushes, &selected_brushes };
    for ( int l = 0; l < 2; ++l )
        for ( selbrush_t *b = lists[l]->next; b && b != lists[l]; b = b->next )
        {
            if ( !b->patch || !b->patch->def )
                continue;
            patchMesh_t *def = b->patch->def;
            stamp = stamp * 6364136223846793005ull
                  + (unsigned long long)(uintptr_t)def + (unsigned long long)def->version;
            if ( !def->curveDef || !def->curveDef->verts )
                anyMissingMesh = true;                     // tessellates later — retry then
        }
    if ( stamp == s_lastStamp )
        return;

    // ── gather perimeter verts of every welded-eligible def (dedup defs) ─────
    std::vector<patchMesh_t *> defs;
    for ( int l = 0; l < 2; ++l )
        for ( selbrush_t *b = lists[l]->next; b && b != lists[l]; b = b->next )
        {
            if ( !b->patch || !b->patch->def )
                continue;
            patchMesh_t *def = b->patch->def;
            if ( !def->curveDef || !def->curveDef->verts )
                continue;
            if ( def->curveDef->width < 2 || def->curveDef->height < 2 )
                continue;
            if ( std::find( defs.begin(), defs.end(), def ) == defs.end() )
                defs.push_back( def );
        }

    std::vector<kpwVert_t> verts;
    for ( size_t d = 0; d < defs.size(); ++d )
        KPW_CollectPerimeter( defs[d], verts );

    // ── group coincident keys; per-member average within the compiler's gate ─
    std::sort( verts.begin(), verts.end(),
               []( const kpwVert_t &a, const kpwVert_t &b ) { return a.key < b.key; } );

    std::vector<patchMesh_t *> touched;
    for ( size_t i = 0; i < verts.size(); )
    {
        size_t j = i + 1;
        while ( j < verts.size() && verts[j].key == verts[i].key )
            ++j;
        if ( j - i >= 2 )
        {
            // Two-phase per group: compute every member's average, then write, so
            // late members do not average against already-welded early members.
            for ( size_t m = i; m < j; ++m )
            {
                const float *nm = verts[m].cv->normal;
                float acc[3] = { 0.0f, 0.0f, 0.0f };
                for ( size_t o = i; o < j; ++o )
                {
                    const float *no = verts[o].cv->normal;
                    const float dot = nm[0]*no[0] + nm[1]*no[1] + nm[2]*no[2];
                    if ( o != m && dot < 0.01f )           // cod4map smoothAngle default
                        continue;
                    acc[0] += no[0]; acc[1] += no[1]; acc[2] += no[2];
                }
                if ( Vec3Normalize_R( acc ) < 0.001f )
                    continue;                              // opposing normals cancelled — keep own
                const float *cur = verts[m].cv->normal;
                if ( fabsf( acc[0] - cur[0] ) + fabsf( acc[1] - cur[1] )
                   + fabsf( acc[2] - cur[2] ) > 0.0001f )
                {
                    verts[m].newNormal[0] = acc[0];
                    verts[m].newNormal[1] = acc[1];
                    verts[m].newNormal[2] = acc[2];
                    verts[m].changed = true;
                }
            }
            for ( size_t m = i; m < j; ++m )
            {
                if ( !verts[m].changed )
                    continue;
                verts[m].cv->normal[0] = verts[m].newNormal[0];
                verts[m].cv->normal[1] = verts[m].newNormal[1];
                verts[m].cv->normal[2] = verts[m].newNormal[2];
                if ( std::find( touched.begin(), touched.end(), verts[m].def ) == touched.end() )
                    touched.push_back( verts[m].def );
            }
        }
        i = j;
    }

    // ── force the lazy instance rebuild for every def whose normals moved ────
    for ( size_t d = 0; d < touched.size(); ++d )
        ++touched[d]->version;

    // Restamp INCLUDING our own version bumps so the pass is one-shot per edit.
    // A def still waiting on tessellation keeps the stamp unstored so it retries.
    if ( !anyMissingMesh )
    {
        stamp = 0x9E3779B97F4A7C15ull;
        for ( int l = 0; l < 2; ++l )
            for ( selbrush_t *b = lists[l]->next; b && b != lists[l]; b = b->next )
            {
                if ( !b->patch || !b->patch->def )
                    continue;
                patchMesh_t *def = b->patch->def;
                stamp = stamp * 6364136223846793005ull
                      + (unsigned long long)(uintptr_t)def + (unsigned long long)def->version;
            }
        s_lastStamp = stamp;
    }
}

extern bool MaterialDef_15_Drawflag_Multiply( int drawFlags, MaterialDef *m );

// ── Patch_Fill_Emit (0x440670 / 0x4405f0) — the draw emit ──
// Emit the patch's tessellated mesh into the surf cache at `techType`.  `face` = PM_FRONT_FACE
// or PM_BACK_FACE.  When `mtlOverride` is set, use it for ALL layers (the d_white patch_wireframe
// under-fill); otherwise use each layer's real material (visArray[L].material).  Per-layer the
// binary's DrawPatches draw gate is NOT here: DrawPatches gates once, before Patch_Fill_Emit, with
// MaterialDef_15_Drawflag_Multiply(drawFlags, patch material). Patch_Fill_Emit then emits every layer.
// Keep that shape: day_water_mud is translucent in D3D state but has toolFlags&0x70 == 0x20, so
// the binary draws it in the first/skip-multiply world pass. A depth-write substitute would skip
// it and the water strip would vanish.
// Returns true when at least one mesh was handed to the surf cache.
//
// ── KIWI-UX (ROUND BK, ITEM 1): `sortKeyBias` — THE ONE ADDED ARGUMENT ──────
// USER REPORT, verbatim: *"When far away, curves render their wireform over their
// surfaces.  This seems like a bug and it's ugly - fix it."*
//
// THE MECHANISM, named rather than guessed.  `R_AddEditorSurfsCmd` sorts the
// pass's surfs with `Editor_SurfCompare` (r_ed_scene.cpp:199-215), whose keys are
// **sortKey, then techType, then firstIndex**.  A patch's FILLED front and its
// unselected `PM_BACK_FACE` wireframe are emitted from the SAME material, so they
// carry the SAME sortKey — and `TECHNIQUE_WIREFRAME_SHADED` (29) is numerically
// GREATER than any world technique (fakelight_normal 24 / _view 25), so the
// wireframe always sorts LAST and draws ON TOP.  Whenever a textured surface is
// coplanar with that wireframe the wire wins the depth compare (equal depth, and
// the tools statemap is not depth-func LESS — if it were, the wireframe could
// never have appeared over anything in the first place).
//
// COPLANAR IS THE NORMAL CASE FOR A "CURVE" IN THIS EDITOR: round AY made the
// loft's **Two-sided** default ON, and RADIANT_KNOWN_ISSUES round AY wrote this
// exact prediction down — *"the suspect is the unselected back-face
// TECHNIQUE_WIREFRAME_SHADED pass of one sheet landing on the other sheet's
// textured front at equal depth — a wireframe overlay, not a depth fight, and it
// is stock DrawPatches behaviour that would need a change there rather than in
// the loft."*  This is that change.
//
// AND WHY IT IS A FUNCTION OF DISTANCE: the wireframe fill mode rasterises
// SCREEN-CONSTANT hairlines.  The tessellated grid's screen density rises as the
// patch shrinks, so far away the lines merge and cover the whole surface ("all
// wire"), while up close the same lines are a sparse grid over the texture
// ("only part of it wire") — which is the two screenshots, in order.
//
// THE FIX IS ORDER, NOT SUPPRESSION.  The back-face pass is emitted with a
// sortKey ONE BELOW the fills', so it draws FIRST and any coplanar texture then
// wins on equal depth.  Where the patch genuinely is alone — a single sheet seen
// from behind, round AY's "a patch has one side" case — nothing else draws at
// that depth and the wireframe is exactly as visible as it always was.  The bias
// stays INSIDE the material's own 100-wide bucket (`Editor_MaterialSortKey` is
// `100 * primarySortKey`, r_ed_scene.cpp:190-193), so no other material's bucket
// can be crossed by it.
static bool Patch_Fill_Emit( patch_t *inst, Material *mtlOverride, int face, int techType,
                             int sortKeyBias = 0 )
{
    if ( !inst->visArray || inst->vertCount <= 0 || !inst->indicesFront )
        return false;
    uint16_t *indexTable = ( face == PM_BACK_FACE ) ? inst->indicesBack : inst->indicesFront;
    bool emitted = false;
    if ( mtlOverride )
    {
        Editor_AddMeshCmd( mtlOverride, techType,
                           Editor_MaterialSortKey( mtlOverride ) + sortKeyBias,
                           inst->vertCount, inst->visArray[0].vertHandle,
                           inst->indexCount, indexTable );
        emitted = true;
    }
    else
    {
        int sortKey = inst->visArray[0].material ? Editor_MaterialSortKey( inst->visArray[0].material ) : 0;
        for ( int L = 0; L < inst->visCount; ++L )
        {
            Material *m = inst->visArray[L].material;
            if ( !m ) continue;
            // KIWI-UX (ROUND BK, ITEM 1): a BIASED pass is FLAT in the layer index —
            // the wireframe is the same hairline grid whatever layer it is emitted
            // from, so giving each layer its own key would only interleave it back
            // between the fills it has to precede.
            Editor_AddMeshCmd( m, techType,
                               sortKeyBias ? ( sortKey + sortKeyBias ) : ( sortKey + L ),
                               inst->vertCount, inst->visArray[L].vertHandle,
                               inst->indexCount, indexTable );
            emitted = true;
        }
    }
    return emitted;
}

// DrawPatches (0x4414E0): filled front, selected white under-fill, back-face
// wireframe, optional patch grid, and control-point tail.
bool DrawPatches( patch_t *inst, const orientation_t *orient, int techType, int drawFlags )
{
    if ( !inst || !inst->def )
        return false;
    // Ensure the tessellated render mesh exists (built at load; safety net).
    if ( !inst->def->curveDef )
    {
        extern curvePatchDef_t *Patch_GenericMesh2( patchMesh_t *p, int layer, int *colMap, int *rowMap );
        inst->def->curveDef = Patch_GenericMesh2( inst->def, g_qeglobals.current_edit_layer, 0, 0 );
        ++inst->def->version;
        if ( !inst->def->curveDef )
            return false;
    }
    Patch_Fill_SyncVersion( inst, orient );              // rebuild if stale (with placement orient)
    if ( !inst->visArray || inst->visCount <= 0 )
        return false;                                    // no realized layer -> wireframe fallback

    // DRAW GATE: after
    //   Patch_Fill_SyncVersion(pm, orient);
    //   if (override || MaterialDef_15_Drawflag_Multiply(drawFlags, &patch->texture))
    //       Patch_Fill_Emit(...)
    // and Patch_Fill_Emit emits every layer. Do not substitute material depth-write here: CoD4 water
    // materials intentionally have depth-write off but toolFlags&0x70 != 0x70, so stock Radiant
    // draws them in the first world pass.
    // DrawPatches reads &def->texture FLAT (layer 0 ALWAYS), it does NOT index by
    // current_edit_layer.  (Only the INSTANCE VISUAL build, Patch_BuildInstanceVisuals, uses
    // the current_edit_layer slot.)  DrawPatchCameraFilled does the same flat read — they
    // must agree, or the white selected-outline pass and the filled pass can disagree on the
    // same patch when the edit layer is not 0.
    MaterialDef *mtldef = (MaterialDef *)&inst->def->texture;
    if ( !MaterialDef_15_Drawflag_Multiply( drawFlags, mtldef ) )
        return true;

    // KIWI Terrain Sculpt paint modes hide the wireframe (both the selected white
    // grid below and the unselected grid pass) by reading the pref as "off".
    extern bool KiwiTerrain_HideWireframe();   // kiwi_terrain.cpp (Tab toggles)
    const int patchWireframe = KiwiTerrain_HideWireframe() ? 0 : g_PrefsDlg->patch_wireframe;
    const bool drawFront = patchWireframe == 0 || ( patchWireframe == 2 && techType != TECHNIQUE_WIREFRAME_SHADED );
    if ( drawFront )
        Patch_Fill_Emit( inst, nullptr, PM_FRONT_FACE, techType );

    if ( ( drawFlags & 1 ) != 0 )
    {
        if ( !drawFront )
            Patch_Fill_Emit( inst, g_qeglobals.d_white, PM_FRONT_FACE, techType );
    }
    else
    {
        // KIWI-UX (ROUND BK, ITEM 1): …at sortKey-1 (see Patch_Fill_Emit).  The
        // emit itself is byte-for-byte 0x441572; only its place in the pass's own
        // sort moved, so a coplanar textured sheet is no longer overdrawn by it.
        // KIWI (TERRAIN): while the sculpt tool owns the wireframe this back-face
        // wireframe run stands down too - one draw call per patch, 1,286 of them on a
        // chunked map, visible only from underneath.
        if ( !KiwiTerrain_HideWireframe() )
            Patch_Fill_Emit( inst, nullptr, PM_BACK_FACE,
                             TECHNIQUE_WIREFRAME_SHADED, -1 );
        if ( patchWireframe )
        {
            extern void DrawPatchesWireframeGrid( patch_t *, GfxColor *,
                                                   const orientation_t *, char, int );
            GfxColor color;
            Byte4PackPixelColor( g_qeglobals.d_savedinfo.colors[23], &color );
            DrawPatchesWireframeGrid( inst, &color, orient, 2, drawFlags );
        }
    }

    extern void Patch_DrawControlPoints( patch_t *, const orientation_t * );
    Patch_DrawControlPoints( inst, orient );
    return true;
}

// ── DrawPatchCameraFilled (0x4415D0) — the 3D-CAMERA tech-29 patch draw ───────
// The selected-camera pass emits d_white at TECHNIQUE_WIREFRAME_SOLID through
// both winding lists after the layer-0 draw gate.
bool DrawPatchCameraFilled( patch_t *inst, const orientation_t *orient, int drawFlags )
{
    bool front = false, back = false;

    if ( inst && inst->def && inst->def->curveDef )
    {
        Patch_Fill_SyncVersion( inst, orient );
        if ( inst->visArray && inst->visCount > 0 )
        {
            // &def->texture, no layer index.
            if ( MaterialDef_15_Drawflag_Multiply( drawFlags, (MaterialDef *)&inst->def->texture ) )
            {                                                // else nothing drawn
                front = Patch_Fill_Emit( inst, g_qeglobals.d_white, PM_FRONT_FACE,
                                         TECHNIQUE_WIREFRAME_SOLID );
                back  = Patch_Fill_Emit( inst, g_qeglobals.d_white, PM_BACK_FACE,
                                         TECHNIQUE_WIREFRAME_SOLID );
            }
        }
    }

    return front || back;
}

// ══════════════════════════════════════════════════════════════════════════════
//  Patch_FindReplaceTexture  (0x449520) — patch (pmesh) face material swap.
//  b = the patch brush DEF; replaceName / findName; flags.  When (flags&2)==0 the
//  patch's current-layer material name must equal findName.  On a match → SetMaterial
//  (replaceName) into &patch->texture[layer] + ++version.  Returns 1 if it replaced.
// ══════════════════════════════════════════════════════════════════════════════
char Patch_FindReplaceTexture( brush_t *b, const char *replaceName,
                               const char *findName, char flags )
{
    iassert( b );          // PMESH.CPP:7120
    iassert( b->patch );   // PMESH.CPP:7121

    patchMesh_t *patch = b->patch;
    patchMesh_material *slot = &patch->texture + g_qeglobals.current_edit_layer;
    if ( ( flags & 2 ) == 0 )
    {
        const char *name = (const char *)Materialdef_GetName( (MaterialDef *)slot );
        if ( _stricmp( name, findName ) )
            return 0;                       // current name != find → skip
    }
    SetMaterial( replaceName, slot );
    ++patch->version;
    return 1;
}

// 0x44cf00 Patch_AllocInstance — the patch analogue of Brush_AddToList, called from
// Brush_AddToList when a brush owns a patch.  The instance version word is seeded to
// def->version-1 (the rebuild trigger, mirroring selbrush.version): without it a fresh
// instance whose version happened to equal def->version would skip its first curveDef
// rebuild.
patch_t *Patch_AllocInstance(patchMesh_t *def)
{
    iassert( def );   // PMESH.CPP:9031
    iassert( def->symbiot );   // PMESH.CPP:9032
    vassert( (def->symbiot->refCount >= 0), "(def->symbiot->refCount) = %i", def->symbiot->refCount );   // PMESH.CPP:9033",

    patch_t *p = (patch_t *)calloc( 1, sizeof( patch_t ) );
    if ( p )
    {
        p->def     = def;
        p->version  = (short)( def->version - 1 );
    }
    return p;
}

// ════════════════════════════════════════════════════════════════════════════
//  KIWI-UX — Patch_KiwiEnsureLmapCoords
//
//  The LIGHTMAP-LAYER arm of Patch_KiwiFinishNewLike (:1542-1548), on its own.
//  Same three statements, same constants, same order:
//      terrain -> Patch_TerrainTexProject( p, 1, 16 )   else Patch_ResampleCurveST( p, 1, 16 )
//      size_of_struct_0x504C = 16      (what Patch_Write serialises, :1146)
//      bDirty = 1                      (the gate that makes it serialise at all)
//
//  It exists because the two workers are file-static ported internals with
//  IDA-pinned float behaviour — the same argument Patch_KiwiFinishNew (:1558) and
//  Patch_KiwiLmapAlign (:2875) both make for their own forwarders — and because
//  the public spellings that would otherwise reach them are wrong for a REPAIR:
//  Patch_Lightmap_Texturing (:2821) walks selected_brushes and opens its own undo
//  bracket, and Patch_Lightmap_Texturing_Sub (:2792) re-lays whichever layer the
//  edit layer currently is, which for a repair must be layer 1 unconditionally.
//
//  The caller is kiwi_material.cpp's KiwiMtl_EnsurePatchLayers, for a patch whose
//  lightmap-layer control texCoords are flat or non-finite — a state no ported
//  creator produces, so no ported caller is changed and no ported behaviour moves.
//
//  It does NOT rebuild curveDef: layer-1 texCoords do not participate in the
//  tessellation geometry, and the callers that need a rebuild (the creators)
//  already do one of their own.
// ════════════════════════════════════════════════════════════════════════════
void Patch_KiwiEnsureLmapCoords( patchMesh_t *p )
{
    if ( !p )
        return;
    const bool terrain = ( p->type & PATCH_TERRAIN ) != 0;
    if ( terrain ) Patch_TerrainTexProject( p, 1, 16.0f );
    else           Patch_ResampleCurveST( p, 1, 16.0f );
    *(float *)&p->size_of_struct_0x504C = 16.0f;
    p->bDirty = 1;
    ++p->version;
}

// ── KIWI Terrain Sculpt / Decals bridge (kiwi_terrain.cpp, kiwi_decal.cpp) ─────
//  The KIWI tools bracket a stroke exactly like the original Alt+LMB paint:
//  Patch_Paint (clear xx22b) -> first touch: Patch_PaintMarkUndo -> release:
//  Patch_PaintFinish (tags the touched defs with the undo id).
void Patch_PaintMarkUndo( patchMesh_t *def )
{
    if ( def && def->pSymbiot )
        Undo_AddPaintedBrush( def->pSymbiot );
}

// ── KIWI Terrain Sculpt: texture + tessellate a freshly built control grid ─────────
//  The Create_Terrain / Patch_GenericMesh tail (naturalize layer 0, project layers 1/2,
//  seed the sample size, build curveDef) for patches the terrain tools construct
//  themselves (split chunks, expander chunks).  `texScale` is the layer-0 natural
//  scale (random_texture_stuff[0].sampleSize, 0.25 when unset).
void Patch_KiwiTextureAndBuild( patchMesh_t *p, float texScale )
{
    if ( !p )
        return;
    if ( texScale <= 0.0f )
        texScale = 0.25f;
    Patch_Naturalize2( p, 0, texScale, texScale );
    const bool terrain = ( p->type & PATCH_TERRAIN ) != 0;
    if ( terrain ) Patch_TerrainTexProject( p, 1, 16.0f );
    else           Patch_ResampleCurveST( p, 1, 16.0f );
    *(float *)&p->size_of_struct_0x504C = 16.0f;
    p->bDirty = 1;
    if ( terrain ) Patch_TerrainTexProject( p, 2, 0.25f );
    else           Patch_ResampleCurveST( p, 2, 0.25f );
    Patch_BuildCurveDef( p );
    ++p->version;
}

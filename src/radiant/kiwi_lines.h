#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Budgeted wrapper around R_Add3DLine -> R_AddCmd_Line3D. Render-command overflow
// silently drops commands, so callers must stop or coarsen when the batch is full.
//
// MATERIAL_COLOR.w is an RGB override weight, not transparency; line alpha stays
// 1 and fades use dimmer RGB. Group equal colours because every colour run emits
// another SetMaterialColor + DrawLines command pair.

// Open the single global batch; flush before another Begin. `maxSegments` is the
// hard budget and `width` is the pixel width passed to R_AddCmd_Line3D.
void KiwiLines_Begin( int maxSegments, int width );

// Colour for subsequently added segments; alpha is always opaque.
void KiwiLines_Color( float r, float g, float b );

// Append one segment; false means invalid endpoints or an exhausted budget.
bool KiwiLines_Add( const float *a, const float *b );

// Emit whatever is pending and close the batch.
void KiwiLines_Flush();

// Segments still allowed in the open batch.
int  KiwiLines_Remaining();

// white_tools backface-culls, so orient each fill triangle independently toward
// the viewer by rewriting `idx` in place; zero-area triangles stay unchanged.
// `xyz` and required `eye` are world-space; vertices are `stride` floats apart.
// Perspective uses the eye point, while parallel projection uses `n · vpn`, so
// the orthographic arm tests the direction toward the viewer (`-vpn`).
void KiwiTris_OrientToEye( const float *xyz, int stride,
                           unsigned short *idx, int idxCount, const float *eye );

// Canonical AABB order: bits 0, 1, and 2 select max x, y, and z; clear bits select min.
#define KIWI_BOX_CORNERS 8
#define KIWI_BOX_EDGES   12

// z-min ring, z-max ring, then the verticals; both rings project as convex loops.
const int KIWI_BOX_EDGE[KIWI_BOX_EDGES][2] =
{
    { 0,1 },{ 1,3 },{ 3,2 },{ 2,0 },      // z min ring
    { 4,5 },{ 5,7 },{ 7,6 },{ 6,4 },      // z max ring
    { 0,4 },{ 1,5 },{ 2,6 },{ 3,7 },      // verticals
};

inline void KiwiBox_Corners( const float *mins, const float *maxs,
                             float out[KIWI_BOX_CORNERS][3] )
{
    for ( int i = 0; i < KIWI_BOX_CORNERS; ++i )
    {
        out[i][0] = ( i & 1 ) ? maxs[0] : mins[0];
        out[i][1] = ( i & 2 ) ? maxs[1] : mins[1];
        out[i][2] = ( i & 4 ) ? maxs[2] : mins[2];
    }
}

// This normal is a lighting input, not geometry. Fixed world +Z keeps overlay
// brightness camera- and face-independent; the ported selected-face batch also
// writes world-space normals (Face_AddWindingToTriBatch, brush.cpp 0x47b86a).
inline void KiwiTris_FillNormal( float *out )
{
    out[0] = 0.0f;
    out[1] = 0.0f;
    out[2] = 1.0f;
}

// MATERIAL_COLOR.w = 1 selects flat RGB while packed vertex colour supplies draw
// alpha. Callers own the surrounding material-colour state and must restore it.
void KiwiTris_FillFlatColor( const float rgba[4] );

// Restore the neutral {0,0,0,0} material colour.
void KiwiTris_FillNeutral();

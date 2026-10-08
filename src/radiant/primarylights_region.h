#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Light-region CSG builder API.

struct winding_t;

// The light's region face node (48 bytes; see primarylights_region.cpp banner).
struct rface_t;
void Region_FreeList( rface_t *list );

// The light descriptor passed to the merge driver: { int class; float p[8] } where
// p[0..2]=origin, p[3..5]=cone axis, p[6]=radius, p[7]=cosHalfFov.  Built by the
// CamWnd region chain (sub_406CE0) from Entity_Light's classification + cone params.
struct lightDesc_t
{
    int   cls;     // +0x00  Entity_Light classification (2=cone, 3=ambient/no-cone)
    float p[8];    // +0x04  [0..2]=origin, [3..5]=dir, [6]=radius, [7]=cosHalfFov
};

// Region_AddWindingFace (0x4DAA70) — build a region face from a world-space winding `w`
// (the light descriptor viewed as floats: light[1..3] = origin, light[7] = radius) and link
// it onto *outList.  `exterior` = the shadow-caster "is exterior" flag.
void Region_AddWindingFace( const winding_t *w, rface_t **outList, const float *light, int exterior );

// Region_AddBoundsFaces (0x4DAD20) — add the light's bounding-cube exterior faces to
// *outList. desc = the region descriptor; *(desc+28) = radius.
void Region_AddBoundsFaces( rface_t **outList, const float *desc );

// Region_BuildHulls (0x4DD260) — decompose *faceList into convex sub-regions, build a
// GfxLightRegionHull for each, append to outHulls[], return the hull count.
int  Region_BuildHulls( rface_t **faceList, const lightDesc_t *desc, void **outHulls );

// Winding_Plane (sub_4D6EF0) — winding best-fit plane: normal in out[0..2], dist in
// out[3]; returns 0.5 * sum-of-cross-magnitudes (the winding area).  Also used by the
// camwnd region-hull draw (sub_40C640).
float Region_WindingPlane( float *out, const winding_t *w );

// Winding_Clip_real_ (polylib 0x4D83B0) — destructive in-place clip keeping the FRONT
// side (dist > epsilon).  *inout freed+replaced (NULL if all-front clipped away).
void Winding_Clip_real_( winding_t **inout, const float *normal, float dist, float epsilon );

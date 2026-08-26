#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// Shared by model-drag placement and both browsers: Test_Ray handles visible
// brushes/patches, then resident XModels get a bounded mesh pass.

struct ray_t;
struct selbrush_t;
struct XModel;

struct kiwiDropHit_t
{
    float point[3];
    float normal[3];
    float dist;
};

bool KiwiDrop_BoundsValid( const float mins[3], const float maxs[3] );
bool KiwiDrop_IsModelEntity( const selbrush_t *node );

// Uses resident state only; a missing XModel falls back to eclass bounds and null outModel.
bool KiwiDrop_GetModelInfo( selbrush_t *node,
                            float mins[3], float maxs[3],
                            float angles[3], float *scale, float origin[3],
                            XModel **outModel = 0 );

// Rotates/scales the eight local bounds corners about the model origin.  The
// returned extents and optional corners are origin-relative world-axis vectors.
bool KiwiDrop_TransformBounds( const float mins[3], const float maxs[3],
                               const float angles[3], float scale,
                               float outMins[3], float outMaxs[3],
                               float ( *outCorners )[3] = 0 );

// Nearest Test_Ray surface or one of the 64 nearest model-AABB candidates.
// Only live drags omit selected models; browser drops may land on them.
bool KiwiDrop_Trace( const ray_t &ray, bool excludeSelectedModels,
                     kiwiDropHit_t *outHit );

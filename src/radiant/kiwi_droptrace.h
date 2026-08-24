#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// Shared geometry trace used by model-drag placement and both model browsers.
// World brushes and patches retain Test_Ray's visibility rules.  Model entities
// add a bounded mesh pass over resident XModel geometry.

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

// Reads only model/entity state already resident in Radiant.  When the XModel is
// not resident, local eclass bounds are returned and *outModel remains null.
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

// Nearest visible hit across Test_Ray's brush/tessellated-patch result and up to
// 64 nearest model-AABB candidates.  Selected models are omitted only for the
// live drag gesture; browser drops may land on selected existing models.
bool KiwiDrop_Trace( const ray_t &ray, bool excludeSelectedModels,
                     kiwiDropHit_t *outHit );

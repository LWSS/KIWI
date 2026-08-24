#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

#include "stdafx.h"
#include "qe3.h"
#include "prefs.h"
#include <xanim/xmodel.h>

#include "kiwi_droptrace.h"
#include "kiwi_pick.h"
#include "kiwi_section.h"
#include "kiwi_shadowcache.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <vector>

extern entity_s *world_entity;                                         // map.cpp
extern void Test_Ray( float *start, float *dir, int contents,
                      edTrace_t *trace, int traceCount );              // select.cpp
extern int Entity_GetVec3ForKey( entity_s_def *entity, float *out,
                                 const char *key );                    // entity.cpp
extern float *AnglesToAxis( float *angles, float ( *axisOut )[3] );    // engine_stubs.cpp
extern char PMESH_RaySegPick( const float *vB, const float *vA, const float *dir,
                              const float *base, const float *origin,
                              float *outDist, float *outU, float *outV ); // pmesh.cpp
extern bool Editor_ModelsEnabled();                                    // camwnd.cpp

namespace
{
    const int KDROP_MODEL_CANDIDATES = 64;
    const float KDROP_RAY_EPS = 1.0e-5f;

    const char *EntityValue( const entity_s_def *def, const char *key )
    {
        if ( !def || !key )
            return "";
        for ( epair_t *ep = def->epairs; ep; ep = ep->next )
            if ( ep->key && ep->value && _stricmp( ep->key, key ) == 0 )
                return ep->value;
        return "";
    }

    entity_s_def *EntityDef( const selbrush_t *node )
    {
        if ( !node || !node->owner || node->owner == world_entity || !node->owner->def )
            return 0;
        return (entity_s_def *)node->owner->def;
    }

    void AxisToWorldDir( const float axis[3][3], const float local[3], float world[3] )
    {
        world[0] = axis[0][0] * local[0] + axis[1][0] * local[1] + axis[2][0] * local[2];
        world[1] = axis[0][1] * local[0] + axis[1][1] * local[1] + axis[2][1] * local[2];
        world[2] = axis[0][2] * local[0] + axis[1][2] * local[1] + axis[2][2] * local[2];
    }

    void WorldToAxisDir( const float axis[3][3], const float world[3], float local[3] )
    {
        local[0] = axis[0][0] * world[0] + axis[0][1] * world[1] + axis[0][2] * world[2];
        local[1] = axis[1][0] * world[0] + axis[1][1] * world[1] + axis[1][2] * world[2];
        local[2] = axis[2][0] * world[0] + axis[2][1] * world[1] + axis[2][2] * world[2];
    }

    bool NormalizeNormal( float normal[3], const float rayDir[3] )
    {
        const float len2 = normal[0] * normal[0]
                         + normal[1] * normal[1]
                         + normal[2] * normal[2];
        if ( !_finite( len2 ) || len2 < 1.0e-12f )
            return false;
        const float invLen = 1.0f / sqrtf( len2 );
        normal[0] *= invLen;
        normal[1] *= invLen;
        normal[2] *= invLen;
        if ( normal[0] * rayDir[0] + normal[1] * rayDir[1]
           + normal[2] * rayDir[2] > 0.0f )
        {
            normal[0] = -normal[0];
            normal[1] = -normal[1];
            normal[2] = -normal[2];
        }
        return true;
    }

    // Slab trace.  When the ray begins inside, the first positive exit surface is
    // returned; the optional lower bound is zero in that case for candidate culling.
    bool RayBounds( const float origin[3], const float dir[3],
                    const float mins[3], const float maxs[3],
                    float *outDist, float outNormal[3],
                    float *outLowerBound = 0 )
    {
        float nearDist = -FLT_MAX;
        float farDist = FLT_MAX;
        float nearNormal[3] = { 0.0f, 0.0f, 0.0f };
        float farNormal[3] = { 0.0f, 0.0f, 0.0f };

        for ( int axis = 0; axis < 3; ++axis )
        {
            if ( fabsf( dir[axis] ) < KDROP_RAY_EPS )
            {
                if ( origin[axis] < mins[axis] || origin[axis] > maxs[axis] )
                    return false;
                continue;
            }

            float t0 = ( mins[axis] - origin[axis] ) / dir[axis];
            float t1 = ( maxs[axis] - origin[axis] ) / dir[axis];
            float n0 = -1.0f;
            float n1 = 1.0f;
            if ( t0 > t1 )
            {
                const float swapT = t0; t0 = t1; t1 = swapT;
                const float swapN = n0; n0 = n1; n1 = swapN;
            }
            if ( t0 > nearDist )
            {
                nearDist = t0;
                nearNormal[0] = nearNormal[1] = nearNormal[2] = 0.0f;
                nearNormal[axis] = n0;
            }
            if ( t1 < farDist )
            {
                farDist = t1;
                farNormal[0] = farNormal[1] = farNormal[2] = 0.0f;
                farNormal[axis] = n1;
            }
            if ( nearDist > farDist )
                return false;
        }

        const bool entry = nearDist > KDROP_RAY_EPS;
        const float dist = entry ? nearDist : farDist;
        if ( !( dist > KDROP_RAY_EPS ) || !_finite( dist ) )
            return false;
        if ( outLowerBound )
            *outLowerBound = entry ? nearDist : 0.0f;
        if ( outDist )
            *outDist = dist;
        if ( outNormal )
        {
            const float *normal = entry ? nearNormal : farNormal;
            outNormal[0] = normal[0];
            outNormal[1] = normal[1];
            outNormal[2] = normal[2];
        }
        return true;
    }

    bool ModelMeshVisible( const selbrush_t *node )
    {
        if ( !node || !node->def || ( node->def->unk01 & 0xFF ) != 0
          || !Editor_ModelsEnabled() )
            return false;
        const int show = g_PrefsDlg->m_nEntityShowState;
        if ( show == 0x1000 )                         // bounding-box mode
            return false;
        if ( ( show & 0x100 ) != 0
          && ( node->brushFlags & BRUSHFLAG_SELECTED ) == 0 )
            return false;                            // selected-mesh modes
        return ( show & ( 0x1 | 0x10 | 0x10000 ) ) != 0;
    }

    struct modelCandidate_t
    {
        entity_s *owner;
        XModel *model;
        float mins[3];
        float maxs[3];
        float origin[3];
        float axis[3][3];
        float scale;
        float boundsDist;
        bool meshVisible;
    };

    bool CandidateAlreadyHeld( const modelCandidate_t *candidates, int count,
                               const entity_s *owner )
    {
        for ( int i = 0; i < count; ++i )
            if ( candidates[i].owner == owner )
                return true;
        return false;
    }

    void KeepCandidate( const modelCandidate_t &candidate,
                        modelCandidate_t *candidates, int *count )
    {
        if ( *count < KDROP_MODEL_CANDIDATES )
        {
            candidates[( *count )++] = candidate;
            return;
        }
        int farthest = 0;
        for ( int i = 1; i < *count; ++i )
            if ( candidates[i].boundsDist > candidates[farthest].boundsDist )
                farthest = i;
        if ( candidate.boundsDist < candidates[farthest].boundsDist )
            candidates[farthest] = candidate;
    }

    void GatherModelCandidates( selbrush_t *head,
                                const float start[3], const float dir[3],
                                float incumbentDist, bool excludeSelectedModels,
                                modelCandidate_t *candidates, int *candidateCount )
    {
        for ( selbrush_t *node = head->next; node && node != head; node = node->next )
        {
            if ( !Pick_BrushPickable( node ) || !KiwiDrop_IsModelEntity( node ) )
                continue;
            if ( excludeSelectedModels
              && ( node->brushFlags & BRUSHFLAG_SELECTED ) != 0 )
                continue;
            if ( CandidateAlreadyHeld( candidates, *candidateCount, node->owner ) )
                continue;

            modelCandidate_t candidate;
            float angles[3];
            if ( !KiwiDrop_GetModelInfo( node, candidate.mins, candidate.maxs,
                                         angles, &candidate.scale, candidate.origin,
                                         &candidate.model ) )
                continue;
            AnglesToAxis( angles, candidate.axis );

            float relativeMins[3], relativeMaxs[3];
            if ( !KiwiDrop_TransformBounds( candidate.mins, candidate.maxs,
                                            angles, candidate.scale,
                                            relativeMins, relativeMaxs ) )
                continue;
            float worldMins[3], worldMaxs[3];
            for ( int axis = 0; axis < 3; ++axis )
            {
                worldMins[axis] = candidate.origin[axis] + relativeMins[axis];
                worldMaxs[axis] = candidate.origin[axis] + relativeMaxs[axis];
            }
            if ( !RayBounds( start, dir, worldMins, worldMaxs,
                             0, 0, &candidate.boundsDist )
              || candidate.boundsDist >= incumbentDist )
                continue;

            candidate.owner = node->owner;
            candidate.meshVisible = ModelMeshVisible( node );
            KeepCandidate( candidate, candidates, candidateCount );
        }
    }

    bool TraceModelCandidate( const modelCandidate_t &candidate,
                              const float start[3], const float dir[3],
                              float *outDist, float outNormal[3] )
    {
        float relativeOrigin[3] = { start[0] - candidate.origin[0],
                                    start[1] - candidate.origin[1],
                                    start[2] - candidate.origin[2] };
        float localOrigin[3], localDir[3];
        WorldToAxisDir( candidate.axis, relativeOrigin, localOrigin );
        WorldToAxisDir( candidate.axis, dir, localDir );
        const float invScale = 1.0f / candidate.scale;
        for ( int axis = 0; axis < 3; ++axis )
            localOrigin[axis] *= invScale;

        const float *verts = 0;
        const unsigned short *indices = 0;
        int indexCount = 0;
        if ( candidate.meshVisible && candidate.model
          && KiwiShadowCache_ModelGeo( candidate.model, &verts, &indices, &indexCount ) )
        {
            float best = FLT_MAX;
            float bestLocalNormal[3] = { 0.0f, 0.0f, 1.0f };
            for ( int i = 0; i + 2 < indexCount; i += 3 )
            {
                const float *a = verts + 3 * indices[i + 0];
                const float *b = verts + 3 * indices[i + 1];
                const float *c = verts + 3 * indices[i + 2];
                float dist;
                if ( PMESH_RaySegPick( c, b, localDir, a, localOrigin,
                                       &dist, 0, 0 )
                  && dist > KDROP_RAY_EPS && _finite( dist ) && dist < best )
                {
                    best = dist;
                    const float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
                    const float e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
                    bestLocalNormal[0] = e1[1] * e2[2] - e1[2] * e2[1];
                    bestLocalNormal[1] = e1[2] * e2[0] - e1[0] * e2[2];
                    bestLocalNormal[2] = e1[0] * e2[1] - e1[1] * e2[0];
                }
            }
            if ( best < FLT_MAX )
            {
                *outDist = best * candidate.scale;
                AxisToWorldDir( candidate.axis, bestLocalNormal, outNormal );
                return NormalizeNormal( outNormal, dir );
            }
            return false;                  // resident mesh: holes remain holes
        }

        float localNormal[3];
        float localDist;
        if ( !RayBounds( localOrigin, localDir, candidate.mins, candidate.maxs,
                         &localDist, localNormal ) )
            return false;
        *outDist = localDist * candidate.scale;
        AxisToWorldDir( candidate.axis, localNormal, outNormal );
        return NormalizeNormal( outNormal, dir );
    }

    struct surfaceUnmask_t
    {
        std::vector<selbrush_t *> nodes;

        surfaceUnmask_t()
        {
            // Selected brush/patch surfaces remain valid browser targets.  Selected
            // models stay masked here and are handled by the explicit model pass.
            for ( selbrush_t *node = selected_brushes.next;
                  node && node != &selected_brushes; node = node->next )
            {
                if ( KiwiDrop_IsModelEntity( node )
                  || ( node->brushFlags & BRUSHFLAG_SELECTED ) == 0 )
                    continue;
                node->brushFlags &= ~(int)BRUSHFLAG_SELECTED;
                nodes.push_back( node );
            }
        }

        ~surfaceUnmask_t()
        {
            for ( size_t i = 0; i < nodes.size(); ++i )
                nodes[i]->brushFlags |= (int)BRUSHFLAG_SELECTED;
        }
    };
}

bool KiwiDrop_BoundsValid( const float mins[3], const float maxs[3] )
{
    if ( !mins || !maxs )
        return false;
    for ( int axis = 0; axis < 3; ++axis )
        if ( !_finite( mins[axis] ) || !_finite( maxs[axis] )
          || !( mins[axis] <= maxs[axis] ) )
            return false;
    return true;
}

bool KiwiDrop_IsModelEntity( const selbrush_t *node )
{
    const entity_s_def *def = EntityDef( node );
    if ( !def )
        return false;
    if ( def->eclass && ( def->eclass->classtype & 0x8 ) != 0 )
        return true;
    return EntityValue( def, "model" )[0] != '\0';
}

bool KiwiDrop_GetModelInfo( selbrush_t *node,
                            float mins[3], float maxs[3],
                            float angles[3], float *scale, float origin[3],
                            XModel **outModel )
{
    entity_s_def *def = EntityDef( node );
    if ( outModel )
        *outModel = 0;
    if ( !def || !KiwiDrop_IsModelEntity( node )
      || !mins || !maxs || !angles || !scale || !origin )
        return false;

    XModel *model = 0;
    bool haveBounds = false;
    entitymodel_t *modelClass = (entitymodel_t *)def->modelClass;
    if ( modelClass && modelClass->model && modelClass->model->handle )
    {
        model = (XModel *)(intptr_t)modelClass->model->handle;
        XModelGetBounds( model, mins, maxs );
        haveBounds = KiwiDrop_BoundsValid( mins, maxs );
        if ( !haveBounds )
            model = 0;
    }
    if ( !haveBounds && def->eclass )
    {
        for ( int axis = 0; axis < 3; ++axis )
        {
            mins[axis] = def->eclass->mins[axis];
            maxs[axis] = def->eclass->maxs[axis];
        }
        haveBounds = KiwiDrop_BoundsValid( mins, maxs );
    }
    if ( !haveBounds )
        return false;

    if ( !Entity_GetVec3ForKey( def, angles, "angles" ) )
        angles[0] = angles[1] = angles[2] = 0.0f;
    *scale = 1.0f;
    const char *modelScale = EntityValue( def, "modelscale" );
    if ( modelScale[0] )
    {
        const float parsed = (float)atof( modelScale );
        if ( _finite( parsed ) && parsed > 0.0f )
            *scale = parsed;
    }
    for ( int axis = 0; axis < 3; ++axis )
        origin[axis] = def->origin[axis];
    if ( outModel )
        *outModel = model;
    return true;
}

bool KiwiDrop_TransformBounds( const float mins[3], const float maxs[3],
                               const float inAngles[3], float scale,
                               float outMins[3], float outMaxs[3],
                               float ( *outCorners )[3] )
{
    if ( !KiwiDrop_BoundsValid( mins, maxs ) || !outMins || !outMaxs )
        return false;
    float angles[3] = { inAngles ? inAngles[0] : 0.0f,
                        inAngles ? inAngles[1] : 0.0f,
                        inAngles ? inAngles[2] : 0.0f };
    float axis[3][3];
    AnglesToAxis( angles, axis );
    if ( !( scale > 0.0f ) || !_finite( scale ) )
        scale = 1.0f;

    outMins[0] = outMins[1] = outMins[2] = FLT_MAX;
    outMaxs[0] = outMaxs[1] = outMaxs[2] = -FLT_MAX;
    for ( int cornerIndex = 0; cornerIndex < 8; ++cornerIndex )
    {
        const float local[3] = {
            ( ( cornerIndex & 1 ) ? maxs[0] : mins[0] ) * scale,
            ( ( cornerIndex & 2 ) ? maxs[1] : mins[1] ) * scale,
            ( ( cornerIndex & 4 ) ? maxs[2] : mins[2] ) * scale };
        float world[3];
        AxisToWorldDir( axis, local, world );
        for ( int component = 0; component < 3; ++component )
        {
            if ( !_finite( world[component] ) )
                return false;
            if ( world[component] < outMins[component] ) outMins[component] = world[component];
            if ( world[component] > outMaxs[component] ) outMaxs[component] = world[component];
            if ( outCorners )
                outCorners[cornerIndex][component] = world[component];
        }
    }
    return true;
}

bool KiwiDrop_Trace( const ray_t &ray, bool excludeSelectedModels,
                     kiwiDropHit_t *outHit )
{
    if ( !outHit )
        return false;
    for ( int axis = 0; axis < 3; ++axis )
        if ( !_finite( ray.origin[axis] ) || !_finite( ray.dir[axis] ) )
            return false;

    float start[3] = { ray.origin[0], ray.origin[1], ray.origin[2] };
    float dir[3] = { ray.dir[0], ray.dir[1], ray.dir[2] };
    KiwiSection_ClampRayStart( start, dir );

    bool found = false;
    float bestDist = FLT_MAX;
    float bestNormal[3] = { 0.0f, 0.0f, 1.0f };

    // Fixed-size/model proxies and camera-excluded flag-0x20 brushes are kept out
    // of this pass.  Patches reach PMESH_51 here, i.e. their tessellated curveDef
    // triangles, not their control hull or symbiont bounds.
    const int contents = ( Pick_CameraContents() | 0x200 ) & ~( 0x400 | 0x1000 );
    edTrace_t worldTrace;
    {
        surfaceUnmask_t unmask;
        Test_Ray( start, dir, contents, &worldTrace, 1 );
    }
    if ( worldTrace.hit.brush && worldTrace.dist > KDROP_RAY_EPS
      && _finite( worldTrace.dist ) )
    {
        float normal[3] = { worldTrace.normal[0], worldTrace.normal[1], worldTrace.normal[2] };
        if ( NormalizeNormal( normal, dir ) )
        {
            found = true;
            bestDist = worldTrace.dist;
            bestNormal[0] = normal[0];
            bestNormal[1] = normal[1];
            bestNormal[2] = normal[2];
        }
    }

    modelCandidate_t candidates[KDROP_MODEL_CANDIDATES];
    int candidateCount = 0;
    if ( !g_PrefsDlg->entities_off )
    {
        GatherModelCandidates( &active_brushes, start, dir, bestDist,
                               excludeSelectedModels, candidates, &candidateCount );
        GatherModelCandidates( &selected_brushes, start, dir, bestDist,
                               excludeSelectedModels, candidates, &candidateCount );
    }

    for ( int i = 0; i < candidateCount; ++i )
    {
        if ( candidates[i].boundsDist >= bestDist )
            continue;
        float dist, normal[3];
        if ( TraceModelCandidate( candidates[i], start, dir, &dist, normal )
          && dist < bestDist )
        {
            found = true;
            bestDist = dist;
            bestNormal[0] = normal[0];
            bestNormal[1] = normal[1];
            bestNormal[2] = normal[2];
        }
    }

    if ( !found )
    {
        // Only a genuinely downward miss receives the editor's global Z=0 fallback.
        if ( !( dir[2] < -KDROP_RAY_EPS ) )
            return false;
        const float dist = -start[2] / dir[2];
        if ( !( dist > KDROP_RAY_EPS ) || !_finite( dist ) )
            return false;
        bestDist = dist;
        bestNormal[0] = 0.0f;
        bestNormal[1] = 0.0f;
        bestNormal[2] = 1.0f;
    }

    outHit->dist = bestDist;
    for ( int axis = 0; axis < 3; ++axis )
    {
        outHit->point[axis] = start[axis] + dir[axis] * bestDist;
        outHit->normal[axis] = bestNormal[axis];
    }
    if ( !found )
        outHit->point[2] = 0.0f;
    return true;
}

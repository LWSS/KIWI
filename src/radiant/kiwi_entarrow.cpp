#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

#include "stdafx.h"
#include "qe3.h"
#include "xywnd.h"
#include "kiwi_entarrow.h"
#include "kiwi_lines.h"
#include "kiwi_walkcache.h"

#include <universal/com_math.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

// KIWI: Verified against entity.cpp's entity-list sentinel definition.
extern entity_s entities;                                                  // entity.cpp:295

namespace
{
    enum { KENTARROW_MAX_ARROWS = 256 };
    enum { KENTARROW_WORLD_SEGMENTS = 5, KENTARROW_XY_SEGMENTS = 3 };

    const float KENTARROW_MIN_LEN = 48.0f;
    const float KENTARROW_XY_DEPTH = 131072.0f;

    enum facingSource_t
    {
        FACING_NONE,
        FACING_TARGET,
        FACING_ANGLES,
        FACING_ANGLE
    };

    struct entArrow_t
    {
        float origin[3];
        float dir[3];
        float len;
        float color[3];
    };

    struct targetResolve_t
    {
        const entity_s_def *source;
        const entity_s_def *target;
    };

    targetResolve_t s_targetCache[KENTARROW_MAX_ARROWS];
    unsigned        s_targetEpoch = ~0u;
    int             s_targetCount = 0;

    const char *FirstKey( const entity_s_def *ent, const char *key )
    {
        if ( !ent || !key )
            return nullptr;
        for ( const epair_t *ep = ent->epairs; ep; ep = ep->next )
        {
            if ( ep->key && !_stricmp( ep->key, key ) )
                return ep->value ? ep->value : "";
        }
        return nullptr;
    }

    bool ParseVec3( const entity_s_def *ent, const char *key, float out[3] )
    {
        const char *value = FirstKey( ent, key );
        return value && sscanf( value, "%f %f %f", out, out + 1, out + 2 ) == 3;
    }

    bool EntityBounds( const entity_s_def *ent, float mins[3], float maxs[3] )
    {
        if ( !ent )
            return false;

        bool have = false;
        const brush_t *sentinel = (const brush_t *)&ent->def;
        for ( const brush_t *b = (const brush_t *)ent->brushes.prev;
              b && b != sentinel; b = b->onext )
        {
            if ( b->maxs[0] < b->mins[0] || b->maxs[1] < b->mins[1]
                 || b->maxs[2] < b->mins[2] )
                continue;
            for ( int k = 0; k < 3; ++k )
            {
                if ( !have || b->mins[k] < mins[k] ) mins[k] = b->mins[k];
                if ( !have || b->maxs[k] > maxs[k] ) maxs[k] = b->maxs[k];
            }
            have = true;
        }

        // KIWI: A fixed-size entity normally has a bbox brush; this keeps the API
        // useful during the short interval before that display proxy is rebuilt.
        if ( !have && ent->eclass && ent->eclass->fixedsize )
        {
            for ( int k = 0; k < 3; ++k )
            {
                mins[k] = ent->origin[k] + ent->eclass->mins[k];
                maxs[k] = ent->origin[k] + ent->eclass->maxs[k];
            }
            have = true;
        }
        return have;
    }

    void EntityOrigin( const entity_s_def *ent, bool haveBounds,
                       const float mins[3], const float maxs[3], float out[3] )
    {
        if ( ent->eclass && ent->eclass->fixedsize )
        {
            out[0] = ent->origin[0];
            out[1] = ent->origin[1];
            out[2] = ent->origin[2];
            return;
        }

        // KIWI: Brush entities honour an explicit origin; otherwise their complete
        // brush-def bounds supply the same stable centre used by selection framing.
        if ( ParseVec3( ent, "origin", out ) )
            return;
        if ( haveBounds )
        {
            out[0] = ( mins[0] + maxs[0] ) * 0.5f;
            out[1] = ( mins[1] + maxs[1] ) * 0.5f;
            out[2] = ( mins[2] + maxs[2] ) * 0.5f;
            return;
        }
        out[0] = ent->origin[0];
        out[1] = ent->origin[1];
        out[2] = ent->origin[2];
    }

    float Normalize( float v[3] )
    {
        const float lenSq = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
        if ( !( lenSq > 1.0e-8f ) )
            return 0.0f;
        const float len = sqrtf( lenSq );
        const float inv = 1.0f / len;
        v[0] *= inv;
        v[1] *= inv;
        v[2] *= inv;
        return len;
    }

    const entity_s_def *FindTarget( const char *target )
    {
        if ( !target || !*target || !entities.next )
            return nullptr;
        for ( const entity_s *it = entities.next; it && it != &entities; it = it->next )
        {
            const entity_s_def *candidate = (const entity_s_def *)it;
            const char *targetname = FirstKey( candidate, "targetname" );
            // KIWI: Connection lines use a case-sensitive value match; facing uses
            // that same Radiant target-resolution convention.
            if ( targetname && !strcmp( targetname, target ) )
                return candidate;
        }
        return nullptr;
    }

    const entity_s_def *ResolveTarget( const entity_s_def *source, const char *target )
    {
        const unsigned epoch = KiwiWalkCache_Epoch();
        if ( epoch != s_targetEpoch )
        {
            s_targetEpoch = epoch;
            s_targetCount = 0;
        }
        for ( int i = 0; i < s_targetCount; ++i )
        {
            if ( s_targetCache[i].source == source )
                return s_targetCache[i].target;
        }

        const entity_s_def *resolved = FindTarget( target );
        // KIWI: Cache resolved and unresolved targets against the editor's global
        // change epoch so camera, XY, and helper-panel queries share one scan.
        if ( s_targetCount < KENTARROW_MAX_ARROWS )
        {
            s_targetCache[s_targetCount].source = source;
            s_targetCache[s_targetCount].target = resolved;
            ++s_targetCount;
        }
        return resolved;
    }

    bool GetFacing( const entity_s_def *ent, float origin[3], float dir[3],
                    float *outLen, facingSource_t *outSource )
    {
        if ( !ent || !origin || !dir || !outLen )
            return false;

        float mins[3] = { 0.0f, 0.0f, 0.0f };
        float maxs[3] = { 0.0f, 0.0f, 0.0f };
        const bool haveBounds = EntityBounds( ent, mins, maxs );
        EntityOrigin( ent, haveBounds, mins, maxs, origin );

        facingSource_t source = FACING_NONE;
        const char *target      = FirstKey( ent, "target" );
        const char *anglesValue = FirstKey( ent, "angles" );
        const char *angleValue  = FirstKey( ent, "angle" );
        if ( target && *target )
        {
            const entity_s_def *targetEnt = ResolveTarget( ent, target );
            if ( targetEnt )
            {
                float targetMins[3] = { 0.0f, 0.0f, 0.0f };
                float targetMaxs[3] = { 0.0f, 0.0f, 0.0f };
                const bool haveTargetBounds = EntityBounds( targetEnt, targetMins, targetMaxs );
                float targetOrigin[3];
                EntityOrigin( targetEnt, haveTargetBounds, targetMins, targetMaxs, targetOrigin );
                dir[0] = targetOrigin[0] - origin[0];
                dir[1] = targetOrigin[1] - origin[1];
                dir[2] = targetOrigin[2] - origin[2];
                if ( Normalize( dir ) == 0.0f )
                    return false;
                source = FACING_TARGET;
            }
        }

        if ( source == FACING_NONE )
        {
            if ( anglesValue )
            {
                float angles[3] = { 0.0f, 0.0f, 0.0f };
                float parsed[3];
                if ( sscanf( anglesValue, "%f %f %f", parsed, parsed + 1, parsed + 2 ) == 3 )
                {
                    angles[0] = parsed[0];
                    angles[1] = parsed[1];
                    angles[2] = parsed[2];
                }
                AngleVectors( angles, dir, nullptr, nullptr );
                if ( Normalize( dir ) == 0.0f )
                    return false;
                source = FACING_ANGLES;
            }
        }

        if ( source == FACING_NONE )
        {
            if ( angleValue )
            {
                float yaw = 0.0f;
                sscanf( angleValue, "%f", &yaw );
                // KIWI: Radiant has no single-key direction reader, so retain the
                // Quake angle convention: -1 is up, -2 is down, otherwise yaw.
                if ( yaw == -1.0f )
                {
                    dir[0] = 0.0f; dir[1] = 0.0f; dir[2] = 1.0f;
                }
                else if ( yaw == -2.0f )
                {
                    dir[0] = 0.0f; dir[1] = 0.0f; dir[2] = -1.0f;
                }
                else
                {
                    const float angles[3] = { 0.0f, yaw, 0.0f };
                    AngleVectors( angles, dir, nullptr, nullptr );
                }
                if ( Normalize( dir ) == 0.0f )
                    return false;
                source = FACING_ANGLE;
            }
        }

        // KIWI: Point-like entities face +X when their implicit angles are 0 0 0.
        if ( source == FACING_NONE && ( !target || !*target )
             && !anglesValue && !angleValue )
        {
            const char *model = FirstKey( ent, "model" );
            const bool pointLike = ( ent->eclass && ent->eclass->fixedsize )
                                || ( model && *model );
            if ( pointLike )
            {
                const float angles[3] = { 0.0f, 0.0f, 0.0f };
                AngleVectors( angles, dir, nullptr, nullptr );
                if ( Normalize( dir ) == 0.0f )
                    return false;
                source = FACING_ANGLES;
            }
        }

        if ( source == FACING_NONE )
            return false;

        float halfExtent = 0.0f;
        if ( haveBounds )
        {
            for ( int k = 0; k < 3; ++k )
            {
                const float half = ( maxs[k] - mins[k] ) * 0.5f;
                if ( half > halfExtent ) halfExtent = half;
            }
        }
        float len = halfExtent * 1.5f;
        if ( len < KENTARROW_MIN_LEN ) len = KENTARROW_MIN_LEN;

        const bool light = ent->eclass && ( ent->eclass->classtype & 0x1 ) != 0;
        if ( light && ( source == FACING_TARGET || source == FACING_ANGLES ) )
        {
            const char *radiusValue = FirstKey( ent, "radius" );
            float radius = 0.0f;
            if ( radiusValue && sscanf( radiusValue, "%f", &radius ) == 1 && radius > len )
                len = radius;
        }

        *outLen = len;
        if ( outSource ) *outSource = source;
        return true;
    }

    float Clamp01( float v )
    {
        if ( v < 0.0f ) return 0.0f;
        if ( v > 1.0f ) return 1.0f;
        return v;
    }

    void ArrowColor( const entity_s_def *ent, float out[3] )
    {
        out[0] = out[1] = out[2] = 1.0f;
        if ( ent && ent->eclass )
        {
            out[0] = ent->eclass->color[0];
            out[1] = ent->eclass->color[1];
            out[2] = ent->eclass->color[2];
        }

        // KIWI: Lights retain their authored hue; `_color` takes precedence over
        // the eclass swatch, matching the existing entity-colour path.
        if ( ent && ent->eclass && ( ent->eclass->classtype & 0x1 ) != 0 )
        {
            float lightColor[3];
            if ( ParseVec3( ent, "_color", lightColor ) )
            {
                out[0] = lightColor[0];
                out[1] = lightColor[1];
                out[2] = lightColor[2];
            }
        }

        out[0] = Clamp01( out[0] );
        out[1] = Clamp01( out[1] );
        out[2] = Clamp01( out[2] );
        float peak = out[0];
        if ( out[1] > peak ) peak = out[1];
        if ( out[2] > peak ) peak = out[2];
        if ( peak > 0.001f && peak < 0.45f )
        {
            const float brighten = 0.45f / peak;
            out[0] = Clamp01( out[0] * brighten );
            out[1] = Clamp01( out[1] * brighten );
            out[2] = Clamp01( out[2] * brighten );
        }
        else if ( peak <= 0.001f )
        {
            out[0] = out[1] = out[2] = 1.0f;
        }
    }

    bool IsWorldspawn( const entity_s_def *ent )
    {
        return ent && ent->eclass && ent->eclass->name
            && !_stricmp( ent->eclass->name, "worldspawn" );
    }

    int GatherArrows( entArrow_t out[KENTARROW_MAX_ARROWS] )
    {
        const entity_s *seen[KENTARROW_MAX_ARROWS];
        int seenCount = 0;
        int arrowCount = 0;

        // KIWI: One selected-list walk and one facing query per unique owner keep
        // brush entities from multiplying arrows or target scans by brush count.
        for ( selbrush_t *b = selected_brushes.next;
              b && b != &selected_brushes; b = b->next )
        {
            const entity_s *owner = b->owner;
            const entity_s_def *ent = owner ? (const entity_s_def *)owner->def : nullptr;
            if ( !owner || !ent || IsWorldspawn( ent ) )
                continue;

            bool duplicate = false;
            for ( int i = 0; i < seenCount; ++i )
            {
                if ( seen[i] == owner )
                {
                    duplicate = true;
                    break;
                }
            }
            if ( duplicate )
                continue;
            if ( seenCount == KENTARROW_MAX_ARROWS )
                break;
            seen[seenCount++] = owner;

            entArrow_t &arrow = out[arrowCount];
            if ( !KiwiEntArrow_GetFacing( ent, arrow.origin, arrow.dir, &arrow.len ) )
                continue;
            ArrowColor( ent, arrow.color );
            if ( ++arrowCount == KENTARROW_MAX_ARROWS )
                break;
        }
        return arrowCount;
    }

    void Cross( const float a[3], const float b[3], float out[3] )
    {
        out[0] = a[1] * b[2] - a[2] * b[1];
        out[1] = a[2] * b[0] - a[0] * b[2];
        out[2] = a[0] * b[1] - a[1] * b[0];
    }

    void EmitWorldArrow( const entArrow_t &arrow )
    {
        float tip[3];
        for ( int k = 0; k < 3; ++k )
            tip[k] = arrow.origin[k] + arrow.dir[k] * arrow.len;
        KiwiLines_Add( arrow.origin, tip );

        const bool nearVertical = fabsf( arrow.dir[2] ) > 0.9f;
        const float ref[3] = { 0.0f, nearVertical ? 1.0f : 0.0f,
                               nearVertical ? 0.0f : 1.0f };
        float side[3], up[3];
        Cross( arrow.dir, ref, side );
        Normalize( side );
        Cross( side, arrow.dir, up );
        Normalize( up );

        float head = arrow.len * 0.22f;
        if ( head > 32.0f ) head = 32.0f;
        const float radius = head * 0.55f;
        float base[3];
        for ( int k = 0; k < 3; ++k )
            base[k] = tip[k] - arrow.dir[k] * head;

        float wing[4][3];
        for ( int k = 0; k < 3; ++k )
        {
            wing[0][k] = base[k] + side[k] * radius;
            wing[1][k] = base[k] - side[k] * radius;
            wing[2][k] = base[k] + up[k] * radius;
            wing[3][k] = base[k] - up[k] * radius;
        }
        for ( int i = 0; i < 4; ++i )
            KiwiLines_Add( tip, wing[i] );
    }

    void EmitXYArrow( const entArrow_t &arrow, int viewType, float scale )
    {
        const int axisH = ( viewType == ED_VIEW_YZ );
        const int axisV = ( viewType != ED_VIEW_XY ) + 1;
        const int axisD = 3 - axisH - axisV;

        const float deltaH = arrow.dir[axisH] * arrow.len;
        const float deltaV = arrow.dir[axisV] * arrow.len;
        const float projectedLen = sqrtf( deltaH * deltaH + deltaV * deltaV );
        if ( !( projectedLen > 1.0e-4f ) )
            return;

        float base[3] = { arrow.origin[0], arrow.origin[1], arrow.origin[2] };
        float tip[3] = { arrow.origin[0], arrow.origin[1], arrow.origin[2] };
        base[axisD] = KENTARROW_XY_DEPTH;
        tip[axisH] += deltaH;
        tip[axisV] += deltaV;
        tip[axisD] = KENTARROW_XY_DEPTH;
        KiwiLines_Add( base, tip );

        const float dirH = deltaH / projectedLen;
        const float dirV = deltaV / projectedLen;
        float head = ( scale > 0.0f ) ? ( 8.0f / scale ) : 8.0f;
        if ( head > projectedLen * 0.35f ) head = projectedLen * 0.35f;
        const float wingRadius = head * 0.65f;

        float wing1[3] = { tip[0], tip[1], tip[2] };
        float wing2[3] = { tip[0], tip[1], tip[2] };
        wing1[axisH] -= dirH * head;
        wing1[axisV] -= dirV * head;
        wing2[axisH] = wing1[axisH];
        wing2[axisV] = wing1[axisV];
        wing1[axisH] -= dirV * wingRadius;
        wing1[axisV] += dirH * wingRadius;
        wing2[axisH] += dirV * wingRadius;
        wing2[axisV] -= dirH * wingRadius;
        KiwiLines_Add( tip, wing1 );
        KiwiLines_Add( tip, wing2 );
    }
}

bool KiwiEntArrow_GetFacing( const entity_s_def *ent, float *outOrigin,
                             float *outDir, float *outLen )
{
    return GetFacing( ent, outOrigin, outDir, outLen, nullptr );
}

void KiwiEntArrow_DrawWorld()
{
    entArrow_t arrows[KENTARROW_MAX_ARROWS];
    const int count = GatherArrows( arrows );
    if ( count <= 0 )
        return;

    KiwiLines_Begin( KENTARROW_MAX_ARROWS * KENTARROW_WORLD_SEGMENTS, 2 );
    for ( int i = 0; i < count; ++i )
    {
        KiwiLines_Color( arrows[i].color[0], arrows[i].color[1], arrows[i].color[2] );
        EmitWorldArrow( arrows[i] );
    }
    KiwiLines_Flush();
}

void KiwiEntArrow_DrawXY( int viewType, float scale )
{
    if ( viewType != ED_VIEW_XY && viewType != ED_VIEW_XZ && viewType != ED_VIEW_YZ )
        return;

    entArrow_t arrows[KENTARROW_MAX_ARROWS];
    const int count = GatherArrows( arrows );
    if ( count <= 0 )
        return;

    KiwiLines_Begin( KENTARROW_MAX_ARROWS * KENTARROW_XY_SEGMENTS, 2 );
    for ( int i = 0; i < count; ++i )
    {
        KiwiLines_Color( arrows[i].color[0], arrows[i].color[1], arrows[i].color[2] );
        EmitXYArrow( arrows[i], viewType, scale );
    }
    KiwiLines_Flush();
}

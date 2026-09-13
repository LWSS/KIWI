#include <universal/q_shared.h>
#include <gfx_d3d/fxprimitives.h>
#include "database.h"
#include "db_effect_layout.h"

void DB64_LoadEffectSamples(FxElemDef *element)
{
    const size_t velocityBytes = ((size_t)element->velIntervalCount + 1) * sizeof(FxElemVelStateSample);
    const size_t visualBytes = ((size_t)element->visStateIntervalCount + 1) * sizeof(FxElemVisStateSample);
    if ((uintptr_t)element->velSamples == UINTPTR_MAX)
    {
        element->velSamples = (FxElemVelStateSample *)DB_AllocStreamPos(15);
        Load_Stream(true, (uint8_t *)element->velSamples, velocityBytes);
    }
    else if (element->velSamples)
    {
        DB64_ConvertOffsetRange((uintptr_t *)&element->velSamples, velocityBytes);
    }
    if ((uintptr_t)element->visSamples == UINTPTR_MAX)
    {
        element->visSamples = (FxElemVisStateSample *)DB_AllocStreamPos(15);
        Load_Stream(true, (uint8_t *)element->visSamples, visualBytes);
    }
    else if (element->visSamples)
    {
        DB64_ConvertOffsetRange((uintptr_t *)&element->visSamples, visualBytes);
    }
}

void DB64_LoadEffectTrail(FxTrailDef *trail, bool atStreamStart)
{
    Load_Stream(atStreamStart, (uint8_t *)trail, sizeof(FxTrailDef));
    if (!DB64_ValidateTrail(trail, false))
    {
        Com_Error(ERR_DROP, "Invalid native effect trail counts");
    }
    if ((uintptr_t)trail->verts == UINTPTR_MAX)
    {
        trail->verts = (FxTrailVertex *)DB_AllocStreamPos(15);
        Load_Stream(true, (uint8_t *)trail->verts, trail->vertCount * sizeof(FxTrailVertex));
    }
    else if (trail->verts)
    {
        DB64_ConvertOffsetRange((uintptr_t *)&trail->verts, trail->vertCount * sizeof(FxTrailVertex));
    }
    if ((uintptr_t)trail->inds == UINTPTR_MAX)
    {
        trail->inds = (uint16_t *)DB_AllocStreamPos(1);
        Load_Stream(true, (uint8_t *)trail->inds, trail->indCount * sizeof(uint16_t));
    }
    else if (trail->inds)
    {
        DB64_ConvertOffsetRange((uintptr_t *)&trail->inds, trail->indCount * sizeof(uint16_t));
    }
    if (!DB64_ValidateTrail(trail, true))
    {
        Com_Error(ERR_DROP, "Native effect trail references an invalid vertex");
    }
}

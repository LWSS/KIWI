#include <universal/q_shared.h>
#include <gfx_d3d/fxprimitives.h>
#include "db_effect_layout.h"
#include <limits.h>

bool DB64_EffectElementCount(const FxEffectDef *effect, size_t *count)
{
    *count = 0;
    if (!effect || !effect->name || effect->elemDefCountLooping < 0 || effect->elemDefCountOneShot < 0 ||
        effect->elemDefCountEmission < 0)
    {
        return false;
    }
    const uint64_t total =
        (uint64_t)effect->elemDefCountLooping + effect->elemDefCountOneShot + effect->elemDefCountEmission;
    if (total > INT_MAX / sizeof(FxElemDef) || (!!effect->elemDefs != (total != 0)))
    {
        return false;
    }
    *count = (size_t)total;
    return true;
}

bool DB64_ValidateEffectElement(const FxElemDef *element)
{
    if (!element || element->elemType >= 11)
    {
        return false;
    }
    if ((element->velIntervalCount && !element->velSamples) || (element->visStateIntervalCount && !element->visSamples))
    {
        return false;
    }
    if (element->elemType == 9 || element->visualCount > 1)
    {
        return (!!element->visuals.array == (element->visualCount != 0));
    }
    // Lights have no material/model/sound pointer, even when visualCount is one.
    if (element->elemType == 6 || element->elemType == 7)
    {
        return !element->visuals.instance.anonymous;
    }
    return (!!element->visuals.instance.anonymous == (element->visualCount != 0));
}

bool DB64_ValidateTrail(const FxTrailDef *trail, bool checkIndices)
{
    if (!trail || trail->vertCount < 0 || trail->vertCount > 65536 || trail->indCount < 0 ||
        trail->indCount > INT_MAX / sizeof(uint16_t) || (!!trail->verts != (trail->vertCount != 0)) ||
        (!!trail->inds != (trail->indCount != 0)))
    {
        return false;
    }
    if (checkIndices)
    {
        for (int i = 0; i < trail->indCount; ++i)
        {
            if (trail->inds[i] >= trail->vertCount)
            {
                return false;
            }
        }
    }
    return true;
}

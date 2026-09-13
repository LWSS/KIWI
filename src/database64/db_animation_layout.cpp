#include <universal/q_shared.h>
#include <xanim/xanim.h>
#include "db_animation_assets.h"
#include <limits.h>
#include <stdio.h>
#include <math.h>

bool DB64_ValidateAnimationHeader(const XAnimParts *parts, char *error, size_t errorSize)
{
    const char *reason = NULL;
    if (!parts || !parts->name)
    {
        reason = "Native animation has no name";
    }
    else
    {
        uint8_t loop, delta, defaultAsset;
        memcpy(&loop, &parts->bLoop, sizeof(uint8_t));
        memcpy(&delta, &parts->bDelta, sizeof(uint8_t));
        memcpy(&defaultAsset, &parts->isDefault, sizeof(uint8_t));
        if (loop > 1 || delta > 1 || defaultAsset > 1 || !isfinite(parts->framerate) || parts->framerate < 0 ||
            !isfinite(parts->frequency) || parts->frequency < 0 ||
            parts->randomDataShortCount > INT_MAX / sizeof(int16_t) || parts->indexCount > INT_MAX / sizeof(uint16_t))
        {
            reason = "Invalid native animation metadata";
        }
        else if ((!!parts->names != (parts->boneCount[9] != 0)) || (!!parts->notify != (parts->notifyCount != 0)) ||
                 (!!parts->dataByte != (parts->dataByteCount != 0)) ||
                 (!!parts->dataShort != (parts->dataShortCount != 0)) ||
                 (!!parts->dataInt != (parts->dataIntCount != 0)) ||
                 (!!parts->randomDataByte != (parts->randomDataByteCount != 0)) ||
                 (!!parts->randomDataShort != (parts->randomDataShortCount != 0)) ||
                 (!!parts->randomDataInt != (parts->randomDataIntCount != 0)) ||
                 (!!parts->indices.data != (parts->indexCount != 0)))
        {
            reason = "Inconsistent native animation arrays";
        }
    }
    if (reason && errorSize)
    {
        snprintf(error, errorSize, "%s", reason);
    }
    return !reason;
}

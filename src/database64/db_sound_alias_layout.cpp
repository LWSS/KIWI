#include <universal/q_shared.h>
#include <sound/snd_public.h>
#include "db_sound_aliases.h"
#include <stdio.h>
#include <math.h>

bool DB64_ValidateSpeakerMap(const SpeakerMap *map, char *error, size_t errorSize)
{
    const char *reason = NULL;
    if (!map || !map->name)
    {
        reason = "Native speaker map has no name";
    }
    else
    {
        uint8_t isDefault;
        memcpy(&isDefault, &map->isDefault, sizeof(uint8_t));
        if (isDefault > 1)
        {
            reason = "Invalid native speaker map boolean";
        }
        for (int i = 0; i < 2; ++i)
        {
            for (int j = 0; j < 2; ++j)
            {
                const MSSChannelMap *channel = &map->channelMaps[i][j];
                if (channel->speakerCount < 0 || channel->speakerCount > 6)
                {
                    reason = "Invalid native speaker count";
                    continue;
                }
                for (int k = 0; k < channel->speakerCount; ++k)
                {
                    const MSSSpeakerLevels *speaker = &channel->speakers[k];
                    if (speaker->numLevels < 0 || speaker->numLevels > 2)
                    {
                        reason = "Invalid native speaker level count";
                        continue;
                    }
                    for (int level = 0; level < speaker->numLevels; ++level)
                    {
                        if (!isfinite(speaker->levels[level]))
                        {
                            reason = "Invalid native speaker level";
                        }
                    }
                }
            }
        }
    }
    if (reason && errorSize)
    {
        snprintf(error, errorSize, "%s", reason);
    }
    return !reason;
}

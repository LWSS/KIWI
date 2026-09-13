#include <universal/q_shared.h>
#include <sound/snd_public.h>
#include "db_sound_assets.h"
#include <stdio.h>
#include <limits.h>

bool DB64_ValidateLoadedSound(const LoadedSound *sound, char *error, size_t errorSize)
{
    const char *reason = NULL;
    if (!sound || !sound->name || !sound->sound.data)
    {
        reason = "Native loaded sound is missing its name or audio data";
    }
    else
    {
        const _AILSOUNDINFO_COD4 *info = &sound->sound.info;
        if (!info->data_len || info->data_len > INT_MAX || !info->rate || !info->samples ||
            (info->channels != 1 && info->channels != 2) || info->format <= 0 || info->bits <= 0 || info->bits > 32)
        {
            reason = "Invalid native loaded sound metadata";
        }
        else if (info->format == 1 && ((info->bits != 8 && info->bits != 16) ||
                                       (uint64_t)info->samples * info->channels * (info->bits / 8) != info->data_len))
        {
            reason = "Native PCM sound length does not match its sample count";
        }
    }
    if (reason && errorSize)
    {
        snprintf(error, errorSize, "%s", reason);
    }
    return !reason;
}

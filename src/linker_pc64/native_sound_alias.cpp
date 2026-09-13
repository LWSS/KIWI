#include <universal/q_shared.h>
#include <universal/com_sndalias.h>
#include "native_sound_alias.h"
#include "native_sound_alias_source.h"
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <errno.h>
#include <limits.h>

static bool Number(const char *text, float fallback, float *out)
{
    *out = fallback;
    if (!text[0])
    {
        return true;
    }
    char *end;
    errno = 0;
    const float value = strtof(text, &end);
    if (end == text || *end || errno == ERANGE || !isfinite(value))
    {
        return false;
    }
    *out = value;
    return true;
}
static bool Integer(const char *text, int *out)
{
    *out = 0;
    if (!text[0])
    {
        return true;
    }
    char *end;
    errno = 0;
    const long value = strtol(text, &end, 10);
    if (end == text || *end || errno == ERANGE || value < 0 || value > INT_MAX)
    {
        return false;
    }
    *out = (int)value;
    return true;
}
static bool Name(const char *name, bool required)
{
    if (!name || (required && !name[0]) || strlen(name) >= 64)
    {
        return false;
    }
    for (const char *c = name; *c; ++c)
    {
        if (!(*c >= 'a' && *c <= 'z') && !(*c >= 'A' && *c <= 'Z') &&
            !(*c >= '0' && *c <= '9') && *c != '_')
        {
            return false;
        }
    }
    return true;
}

bool Linker_SoundAliasMatches(const char *loadspec, const char *level, const char *game, bool *keep,
                               char *error, size_t errorSize)
{
    if (!loadspec || !level || !game || !keep || (!error && errorSize))
    {
        return false;
    }
    *keep = false;
    if (!loadspec[0])
    {
        *keep = *game != '!';
        return true;
    }
    const bool positiveLevel = *loadspec != '!';
    const bool positiveGame = *game != '!';
    if (!positiveLevel)
    {
        ++loadspec;
    }
    if (!positiveGame)
    {
        ++game;
    }
    bool allSp = false, allMp = false, menu = false, namedLevel = false;
    bool matchLevel = false, matchGame = false, valid = true, any = false;
    while (*loadspec && valid)
    {
        while (*loadspec == ' ' || *loadspec == '\t')
        {
            ++loadspec;
        }
        if (!*loadspec)
        {
            break;
        }
        char token[256];
        size_t size = 0;
        while (*loadspec && *loadspec != ' ' && *loadspec != '\t')
        {
            if (*loadspec == '!' || size + 1 >= sizeof(token))
            {
                valid = false;
                break;
            }
            token[size++] = *loadspec++;
        }
        token[size] = 0;
        any = true;
        if (!_stricmp(token, "all_sp") || !_stricmp(token, "all_mp"))
        {
            allSp |= !_stricmp(token, "all_sp");
            allMp |= !_stricmp(token, "all_mp");
            matchGame |= !_stricmp(token, game);
        }
        else
        {
            menu |= !_stricmp(token, "menu");
            namedLevel |= _stricmp(token, "menu") != 0;
            matchLevel |= !_stricmp(token, level);
        }
    }
    if (!valid || !any || (allSp && allMp) || (menu && (allSp || allMp || namedLevel)))
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid sound alias load specification");
        }
        return false;
    }
    if (menu)
    {
        *keep = (matchLevel || !*level) ? positiveLevel : !positiveLevel;
    }
    else if (matchLevel)
    {
        *keep = positiveLevel;
    }
    else if (matchGame)
    {
        *keep = positiveGame && positiveLevel;
    }
    else if (namedLevel)
    {
        *keep = *level && _stricmp(level, "menu") && !positiveLevel;
    }
    else
    {
        *keep = positiveGame && (positiveLevel ? (!allSp && !allMp) : (allSp || allMp));
    }
    return true;
}

bool Linker_BuildSoundAlias(const LinkerSoundAliasRow *row, int channel, float volumeMod, SoundFile *file,
                            SndCurve *curve, SpeakerMap *speakers, snd_alias_t *alias, int *sequence,
                            char *error, size_t errorSize)
{
    if (!alias || !sequence || !row || (!error && errorSize))
    {
        return false;
    }
    memset(alias, 0, sizeof(snd_alias_t));
    *sequence = 0;
    const char *const *f = row->fields;
    for (int i = 0; i < SA_NUMFIELDS; ++i)
    {
        if (!f[i])
        {
            return false;
        }
    }
    snd_alias_t value = {};
    int order = 0;
    bool valid = channel >= 0 && channel < 64 && isfinite(volumeMod) && volumeMod >= 0 && file && curve && speakers &&
                 Name(f[SA_NAME], true) && Name(f[SA_SECONDARYALIASNAME], false) && Name(f[SA_CHAINALIASNAME], false) &&
                 f[SA_FILE][0] && strlen(f[SA_FILE]) < 64;
    valid = valid && Integer(f[SA_SEQUENCE], &order) && Integer(f[SA_STARTDELAY], &value.startDelay) &&
            Number(f[SA_VOL_MIN], 1, &value.volMin) && Number(f[SA_VOL_MAX], value.volMin, &value.volMax) &&
            Number(f[SA_PITCH_MIN], 1, &value.pitchMin) && Number(f[SA_PITCH_MAX], value.pitchMin, &value.pitchMax) &&
            Number(f[SA_DIST_MIN], 120, &value.distMin) && Number(f[SA_DIST_MAX], 0, &value.distMax) &&
            Number(f[SA_PROBABILITY], 1, &value.probability) && Number(f[SA_LFEPERCENTAGE], 0, &value.lfePercentage) &&
            Number(f[SA_CENTERPERCENTAGE], 0, &value.centerPercentage);
    valid = valid && value.volMin >= 0 && value.volMin <= 1 && value.volMax >= 0 && value.volMax <= 1 &&
            value.pitchMin > 0 && value.pitchMax > 0 && value.probability >= 0 &&
            value.lfePercentage >= 0 && value.lfePercentage <= 1 && value.centerPercentage >= 0 && value.centerPercentage <= 1;
    if (value.volMin > value.volMax)
    {
        const float swap = value.volMin; value.volMin = value.volMax; value.volMax = swap;
    }
    if (value.pitchMin > value.pitchMax)
    {
        const float swap = value.pitchMin; value.pitchMin = value.pitchMax; value.pitchMax = swap;
    }
    if (!value.distMax)
    {
        value.distMax = value.distMin * 5;
    }
    valid = valid && value.distMin > 0 && isfinite(value.distMax) && value.distMax > value.distMin;
    value.volMin = (float)fmin((double)value.volMin * volumeMod, 1.0);
    value.volMax = (float)fmin((double)value.volMax * volumeMod, 1.0);
    int type = SAT_LOADED;
    if (f[SA_TYPE][0])
    {
        if (!_stricmp(f[SA_TYPE], "streamed"))
        {
            type = SAT_STREAMED;
        }
        else if (_stricmp(f[SA_TYPE], "loaded"))
        {
            valid = false;
        }
    }
    valid = valid && file && file->type == type;
    value.flags = ((channel >= 0 && channel < 64 ? channel : 0) << 8) | (type << 6);
    if (!_stricmp(f[SA_LOOP], "looping"))
    {
        value.flags |= 1;
    }
    else if (!_stricmp(f[SA_LOOP], "rlooping"))
    {
        value.flags |= 1 | 32;
    }
    else if (f[SA_LOOP][0] && _stricmp(f[SA_LOOP], "nonlooping"))
    {
        valid = false;
    }
    value.slavePercentage = 1;
    if (!_stricmp(f[SA_MASTERSLAVE], "master"))
    {
        value.flags |= 2;
    }
    else if (f[SA_MASTERSLAVE][0])
    {
        value.flags |= 4;
        valid = valid && Number(f[SA_MASTERSLAVE], 1, &value.slavePercentage) &&
                value.slavePercentage >= 0 && value.slavePercentage <= 1;
    }
    if (strstr(f[SA_REVERB], "fulldrylevel"))
    {
        value.flags |= 8;
    }
    if (strstr(f[SA_REVERB], "nowetlevel"))
    {
        value.flags |= 16;
    }
    if (!valid)
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid sound alias '%s' on source line %u", f[SA_NAME], row->line);
        }
        return false;
    }
    value.aliasName = f[SA_NAME];
    value.subtitle = f[SA_SUBTITLE][0] ? f[SA_SUBTITLE] : NULL;
    value.secondaryAliasName = f[SA_SECONDARYALIASNAME][0] ? f[SA_SECONDARYALIASNAME] : NULL;
    value.chainAliasName = f[SA_CHAINALIASNAME][0] ? f[SA_CHAINALIASNAME] : NULL;
    value.soundFile = file;
    value.volumeFalloffCurve = curve;
    value.speakerMap = speakers;
    *alias = value;
    *sequence = order;
    return true;
}

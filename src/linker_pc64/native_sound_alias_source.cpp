#include <universal/q_shared.h>
#include <universal/com_sndalias.h>
#include "native_sound_alias_source.h"
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <math.h>

static bool ConfigToken(const char **cursor, char *token, size_t capacity)
{
    for (;;)
    {
        while (**cursor && (unsigned char)**cursor <= ' ')
        {
            ++*cursor;
        }
        if (**cursor == '#' || ((*cursor)[0] == '/' && (*cursor)[1] == '/'))
        {
            while (**cursor && **cursor != '\n')
            {
                ++*cursor;
            }
        }
        else
        {
            break;
        }
    }
    size_t count = 0;
    const bool quoted = **cursor == '"';
    if (quoted)
    {
        ++*cursor;
    }
    while (**cursor && (quoted ? **cursor != '"' : ((unsigned char)**cursor > ' ' && **cursor != ',')))
    {
        if (count + 1 >= capacity)
        {
            token[count] = 0;
            return false;
        }
        token[count++] = *(*cursor)++;
    }
    if (quoted)
    {
        if (**cursor != '"')
        {
            token[count] = 0;
            return false;
        }
        ++*cursor;
    }
    token[count] = 0;
    return true;
}

void Linker_DefaultSpeakerMap(SpeakerMap *map)
{
    memset(map, 0, sizeof(SpeakerMap));
    map->name = "default";
    map->isDefault = true;
    for (int output = 0; output < 2; ++output)
    {
        for (int source = 0; source < 2; ++source)
        {
            MSSChannelMap *channels = &map->channelMaps[source][output];
            channels->speakerCount = output ? 6 : 2;
            for (int speaker = 0; speaker < channels->speakerCount; ++speaker)
            {
                MSSSpeakerLevels *levels = &channels->speakers[speaker];
                levels->speaker = speaker;
                levels->numLevels = source + 1;
                if (speaker < 2)
                {
                    if (source)
                    {
                        levels->levels[speaker] = 1;
                    }
                    else
                    {
                        levels->levels[0] = 0.5f;
                    }
                }
            }
        }
    }
}

bool Linker_ReadSpeakerMap(const void *data, size_t size, const char *name, SpeakerMap **map,
                            char *error, size_t errorSize)
{
    if (!map || (!error && errorSize))
    {
        return false;
    }
    *map = NULL;
    bool valid = data && size >= 7 && size <= 8192 && name && name[0] && strlen(name) < 64 &&
                 !memcmp(data, "SPKRMAP", 7) && !memchr(data, 0, size);
    SpeakerMap result = {};
    if (valid)
    {
        char text[8193], token[64];
        memcpy(text, data, size);
        text[size] = 0;
        const char *cursor = text + 7;
        const char *outputs[] = {"LEFTSPEAKER", "RIGHTSPEAKER", "CENTERSPEAKER", "LFESPEAKER",
                                 "LEFTSURROUNDSPEAKER", "RIGHTSURROUNDSPEAKER"};
        for (int output = 0; valid && output < 2; ++output)
        {
            for (int source = 0; valid && source < 2; ++source)
            {
                MSSChannelMap *channels = &result.channelMaps[source][output];
                channels->speakerCount = output ? 6 : 2;
                for (int speaker = 0; valid && speaker < channels->speakerCount; ++speaker)
                {
                    MSSSpeakerLevels *levels = &channels->speakers[speaker];
                    levels->speaker = speaker;
                    levels->numLevels = source + 1;
                    for (int input = 0; valid && input <= source; ++input)
                    {
                        const char *expected = source ? (input ? "RIGHTSOURCE" : "LEFTSOURCE") : "MONOSOURCE";
                        valid = ConfigToken(&cursor, token, sizeof(token)) && !_stricmp(token, expected) &&
                                ConfigToken(&cursor, token, sizeof(token)) && !_stricmp(token, outputs[speaker]) &&
                                ConfigToken(&cursor, token, sizeof(token));
                        char *end;
                        errno = 0;
                        const float gain = strtof(token, &end);
                        valid = valid && end != token && !*end && errno != ERANGE && isfinite(gain) && gain >= 0 && gain <= 1;
                        levels->levels[input] = gain;
                    }
                }
            }
        }
        if (valid)
        {
            valid = ConfigToken(&cursor, token, sizeof(token));
            if (valid && !strcmp(token, "}"))
            {
                valid = ConfigToken(&cursor, token, sizeof(token));
            }
            valid = valid && !token[0];
        }
    }
    if (!valid)
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid speaker-map source");
        }
        return false;
    }
    SpeakerMap *owned = (SpeakerMap *)malloc(sizeof(SpeakerMap) + strlen(name) + 1);
    if (!owned)
    {
        return false;
    }
    *owned = result;
    char *ownedName = (char *)(owned + 1);
    strcpy(ownedName, name);
    owned->name = ownedName;
    *map = owned;
    return true;
}

int Linker_FindSoundChannel(const LinkerSoundAliasConfig *config, const char *name)
{
    if (!name[0])
    {
        return config->channelCount ? 0 : -1;
    }
    for (unsigned int i = 0; i < config->channelCount; ++i)
    {
        if (!_stricmp(config->channels[i], name))
        {
            return (int)i;
        }
    }
    return -1;
}

bool Linker_FindSoundVolume(const LinkerSoundAliasConfig *config, const char *name, float *value)
{
    if (!name[0])
    {
        *value = 1;
        return true;
    }
    for (unsigned int i = 0; i < config->volumeCount; ++i)
    {
        if (!_stricmp(config->volumeGroups[i], name))
        {
            *value = config->volumeValues[i];
            return true;
        }
    }
    return false;
}

bool Linker_ReadSoundAliasConfig(const void *channels, size_t channelSize, const void *volumes, size_t volumeSize,
                                  LinkerSoundAliasConfig *config, char *error, size_t errorSize)
{
    if (!config || (!error && errorSize))
    {
        return false;
    }
    memset(config, 0, sizeof(LinkerSoundAliasConfig));
    bool valid = channels && volumes && channelSize && channelSize <= 16384 && volumeSize >= 15 &&
                 volumeSize <= 8192 && !memchr(channels, 0, channelSize) && !memchr(volumes, 0, volumeSize) &&
                 !memcmp(volumes, "VOLUMEMODGROUPS", 15);
    LinkerSoundAliasConfig result = {};
    if (valid)
    {
        char channelText[16385], volumeText[8193], token[64];
        memcpy(channelText, channels, channelSize);
        channelText[channelSize] = 0;
        memcpy(volumeText, volumes, volumeSize);
        volumeText[volumeSize] = 0;
        const char *cursor = channelText;
        while (valid && *cursor)
        {
            valid = ConfigToken(&cursor, token, sizeof(token));
            if (!valid)
            {
                break;
            }
            if (token[0])
            {
                valid = result.channelCount < 64 && Linker_FindSoundChannel(&result, token) < 0;
                if (valid)
                {
                    strcpy(result.channels[result.channelCount++], token);
                }
            }
            while (*cursor && *cursor != '\n')
            {
                ++cursor;
            }
        }
        valid = valid && result.channelCount != 0;
        cursor = volumeText + 15;
        while (valid && *cursor)
        {
            valid = ConfigToken(&cursor, token, sizeof(token));
            if (!valid || !token[0])
            {
                valid = valid && !*cursor;
                break;
            }
            float ignored;
            valid = result.volumeCount < 32 && !Linker_FindSoundVolume(&result, token, &ignored);
            if (!valid)
            {
                break;
            }
            strcpy(result.volumeGroups[result.volumeCount], token);
            valid = ConfigToken(&cursor, token, sizeof(token)) && token[0];
            char *end;
            errno = 0;
            const float value = strtof(token, &end);
            valid = valid && end != token && !*end && errno != ERANGE && isfinite(value) && value >= 0;
            if (valid)
            {
                result.volumeValues[result.volumeCount++] = value;
            }
        }
    }
    if (!valid)
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid sound channel or volume-group configuration");
        }
        return false;
    }
    *config = result;
    return true;
}

void Linker_FreeSoundAliasSource(LinkerSoundAliasSource *source)
{
    if (source)
    {
        free(source->text);
        free(source->rows);
        free(source);
    }
}

bool Linker_ReadSoundAliasSource(const void *data, size_t size, LinkerSoundAliasSource **source,
                                  char *error, size_t errorSize)
{
    if (!source || (!error && errorSize))
    {
        return false;
    }
    *source = NULL;
    if (!data || !size || size > 16 * 1024 * 1024 || memchr(data, 0, size))
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid sound alias CSV input");
        }
        return false;
    }
    LinkerSoundAliasSource *result = (LinkerSoundAliasSource *)calloc(1, sizeof(LinkerSoundAliasSource));
    if (!result)
    {
        return false;
    }
    result->text = (char *)malloc(size + 1);
    if (!result->text)
    {
        Linker_FreeSoundAliasSource(result);
        return false;
    }
    memcpy(result->text, data, size);
    result->text[size] = 0;
    size_t read = 0, write = 0;
    unsigned int line = 1, headerCount = 0;
    int mapping[128] = {};
    bool header = false, valid = true;
    while (valid && read < size)
    {
        const unsigned int rowLine = line;
        const char *columns[128];
        unsigned int columnCount = 0;
        bool more = true;
        while (valid && more)
        {
            if (columnCount == ARRAY_COUNT(columns))
            {
                valid = false;
                break;
            }
            while (read < size && (result->text[read] == ' ' || result->text[read] == '\t'))
            {
                ++read;
            }
            const size_t begin = write;
            columns[columnCount++] = result->text + write;
            if (read < size && result->text[read] == '"')
            {
                ++read;
                bool closed = false;
                while (read < size)
                {
                    const char c = result->text[read++];
                    if (c == '"')
                    {
                        if (read < size && result->text[read] == '"')
                        {
                            ++read;
                        }
                        else
                        {
                            closed = true;
                            break;
                        }
                    }
                    if (c == '\n')
                    {
                        ++line;
                    }
                    result->text[write++] = c;
                }
                while (read < size && (result->text[read] == ' ' || result->text[read] == '\t'))
                {
                    ++read;
                }
                valid = closed;
            }
            else
            {
                while (read < size && result->text[read] != ',' && result->text[read] != '\r' && result->text[read] != '\n')
                {
                    if (result->text[read] == '"')
                    {
                        valid = false;
                        break;
                    }
                    result->text[write++] = result->text[read++];
                }
                while (write > begin && (result->text[write - 1] == ' ' || result->text[write - 1] == '\t'))
                {
                    --write;
                }
            }
            const char delimiter = read < size ? result->text[read++] : 0;
            more = delimiter == ',';
            if (delimiter == '\r')
            {
                if (read < size && result->text[read] == '\n')
                {
                    ++read;
                }
                ++line;
            }
            else if (delimiter == '\n')
            {
                ++line;
            }
            else if (delimiter && delimiter != ',')
            {
                valid = false;
            }
            result->text[write++] = 0;
        }
        if (!valid || !columns[0][0] || columns[0][0] == '#')
        {
            continue;
        }
        if (!header)
        {
            bool found[SA_NUMFIELDS] = {};
            headerCount = columnCount;
            for (unsigned int c = 0; valid && c < columnCount; ++c)
            {
                for (int field = 1; field < SA_NUMFIELDS; ++field)
                {
                    if (!_stricmp(columns[c], g_pszSndAliasKeyNames[field]))
                    {
                        valid = !found[field];
                        found[field] = true;
                        mapping[c] = field;
                        break;
                    }
                }
            }
            valid = valid && found[SA_NAME] && found[SA_FILE];
            header = true;
            continue;
        }
        // The platform column belongs to XMAUpdate's audio conversion stage.
        // Retail PC zones still include aliases marked !PC (for example menu music).
        if (result->count == 65536)
        {
            valid = false;
            break;
        }
        LinkerSoundAliasRow *rows = (LinkerSoundAliasRow *)realloc(result->rows, (result->count + 1) * sizeof(LinkerSoundAliasRow));
        if (!rows)
        {
            valid = false;
            break;
        }
        result->rows = rows;
        LinkerSoundAliasRow *row = &rows[result->count++];
        row->line = rowLine;
        for (int field = 0; field < SA_NUMFIELDS; ++field)
        {
            row->fields[field] = "";
        }
        for (unsigned int c = 0; c < columnCount && c < headerCount; ++c)
        {
            if (mapping[c])
            {
                row->fields[mapping[c]] = columns[c];
            }
        }
    }
    if (!valid || !header)
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid sound alias CSV near line %u", line);
        }
        Linker_FreeSoundAliasSource(result);
        return false;
    }
    *source = result;
    return true;
}

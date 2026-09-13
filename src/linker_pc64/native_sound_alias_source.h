#pragma once
#include <stddef.h>
struct LinkerSoundAliasRow
{
    // Indexed by snd_alias_members_t; unknown CSV columns are ignored.
    const char *fields[29];
    unsigned int line;
};
struct LinkerSoundAliasSource
{
    char *text;
    LinkerSoundAliasRow *rows;
    unsigned int count;
};
bool Linker_ReadSoundAliasSource(const void *data, size_t size, LinkerSoundAliasSource **source,
                                  char *error, size_t errorSize);
void Linker_FreeSoundAliasSource(LinkerSoundAliasSource *source);

struct LinkerSoundAliasConfig
{
    char channels[64][64];
    unsigned int channelCount;
    char volumeGroups[32][64];
    float volumeValues[32];
    unsigned int volumeCount;
};
bool Linker_ReadSoundAliasConfig(const void *channels, size_t channelSize, const void *volumes, size_t volumeSize,
                                  LinkerSoundAliasConfig *config, char *error, size_t errorSize);
int Linker_FindSoundChannel(const LinkerSoundAliasConfig *config, const char *name);
bool Linker_FindSoundVolume(const LinkerSoundAliasConfig *config, const char *name, float *value);
struct SpeakerMap;
void Linker_DefaultSpeakerMap(SpeakerMap *map);
// Free the returned map with free(); its name is in the same allocation.
bool Linker_ReadSpeakerMap(const void *data, size_t size, const char *name, SpeakerMap **map,
                            char *error, size_t errorSize);

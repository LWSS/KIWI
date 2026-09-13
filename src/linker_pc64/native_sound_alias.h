#pragma once
#include <stddef.h>
struct LinkerSoundAliasRow;
struct SoundFile;
struct SndCurve;
struct SpeakerMap;
struct snd_alias_t;
// Strings and dependencies remain borrowed from the compiler's source/cache.
// sequence is the source sorting key; the runtime alias sequence starts at zero.
bool Linker_BuildSoundAlias(const LinkerSoundAliasRow *row, int channel, float volumeMod, SoundFile *file,
                            SndCurve *curve, SpeakerMap *speakers, snd_alias_t *alias, int *sequence,
                            char *error, size_t errorSize);
bool Linker_SoundAliasMatches(const char *loadspec, const char *level, const char *game, bool *keep,
                               char *error, size_t errorSize);

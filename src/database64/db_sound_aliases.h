#pragma once
#include <stddef.h>
struct SpeakerMap;
union XAssetHeader;
bool DB64_ValidateSpeakerMap(const SpeakerMap *map, char *error, size_t errorSize);
void DB64_LoadSoundAliases(XAssetHeader *header, bool atStreamStart);

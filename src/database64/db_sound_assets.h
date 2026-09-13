#pragma once
#include <stddef.h>
struct LoadedSound;
union XAssetHeader;
bool DB64_ValidateLoadedSound(const LoadedSound *sound, char *error, size_t errorSize);
void DB64_LoadSoundAsset(XAssetHeader *header, bool atStreamStart);

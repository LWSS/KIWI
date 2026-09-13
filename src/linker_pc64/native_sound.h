#pragma once
#include <stddef.h>
struct LoadedSound;
// Import uncompressed 8/16-bit mono/stereo RIFF WAVE without a Miles runtime.
bool Linker_ImportPcmWave(const void *data, size_t size, const char *name, LoadedSound **sound, char *error,
                          size_t errorSize);
void Linker_FreeSound(LoadedSound *sound);
struct SndCurve;
// The result and its name share one allocation; release with free().
bool Linker_ImportSoundCurve(const void *data, size_t size, const char *name, SndCurve **curve,
                             char *error, size_t errorSize);

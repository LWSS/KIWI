#pragma once
#include <stddef.h>
struct LinkerMaterialTexture;
struct water_t;
// Builds a deterministic full-resolution FFT setup and its dynamic L8 image descriptor.
bool Linker_CompileWater(const LinkerMaterialTexture *source, const char *imageName, water_t **water, char *error,
                         size_t errorSize);
void Linker_FreeWater(water_t *water);

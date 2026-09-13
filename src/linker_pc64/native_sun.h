#pragma once
#include <stddef.h>
#include <gfx_d3d/r_sky.h>
struct LinkerSunSettings
{
    sunflare_t sun;
    char sprite[1024], flare[1024];
};
// Material names are returned separately for dependency compilation. Output is unchanged on failure.
bool Linker_ParseSunSettings(const void *data, size_t size, LinkerSunSettings *settings, char *error, size_t errorSize);
struct LinkerCompiledMaterial;
struct LinkerCompiledSun
{
    sunflare_t sun;
    LinkerCompiledMaterial *sprite, *flare;
};
// Missing optional sun files return success with a null result; present invalid files/dependencies fail.
bool Linker_CompileSun(const char *root, const char *mapName, LinkerCompiledSun **result, char *error,
                       size_t errorSize);
void Linker_FreeCompiledSun(LinkerCompiledSun *sun);

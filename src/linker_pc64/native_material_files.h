#pragma once
#include <stddef.h>
struct Material;
struct GfxImage;
struct water_t;
struct LinkerCompiledTechniqueSet;
struct LinkerCompiledMaterial
{
    Material *material;
    LinkerCompiledTechniqueSet *techniques;
    GfxImage *images[255];
    unsigned int imageCount;
    water_t *waters[255];
    unsigned int waterCount;
};
bool Linker_CompileMaterialFiles(const char *root, const char *name, unsigned char imageTrack,
                                 LinkerCompiledMaterial **compiled, char *error, size_t errorSize,
                                 const char *language = NULL);
void Linker_FreeCompiledMaterial(LinkerCompiledMaterial *compiled);

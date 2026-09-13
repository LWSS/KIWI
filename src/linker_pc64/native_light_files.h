#pragma once
#include <stddef.h>
struct GfxLightDef;
struct GfxImage;
// The returned definition owns its name and attenuation image. lookupStart includes the left border pixel.
bool Linker_CompileLightDefinition(const char *root, const char *name, unsigned int lookupStart,
                                   GfxLightDef **definition, char *error, size_t errorSize);
void Linker_FreeLightDefinition(GfxLightDef *definition);
// Writes the first attenuation row and duplicate edge pixels into an unmerged secondary lightmap.
bool Linker_StampLightAttenuation(const GfxLightDef *definition, GfxImage *secondary, char *error, size_t errorSize);

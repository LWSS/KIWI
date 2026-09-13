#pragma once
#include <stddef.h>
struct GfxWorld;
struct LinkerWorldSource;
struct LinkerAssembledWorld;
// Owns the world header, copied cells/portals, outdoor image and runtime arrays; asset geometry remains borrowed.
// Sunflare, special surface transforms and remaining renderer finalization precede serialization.
bool Linker_AssembleWorldGeometry(const LinkerWorldSource *source, const char *name, LinkerAssembledWorld **result,
                                  char *error, size_t errorSize);
GfxWorld *Linker_GetAssembledWorld(LinkerAssembledWorld *world);
void Linker_FreeAssembledWorld(LinkerAssembledWorld *world);

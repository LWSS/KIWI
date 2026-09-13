#pragma once
#include <stddef.h>
struct LinkerBspRenderGeometry;
// Encodes connected magic-portal component centers into UVs. Failure preserves the original vertex array.
bool Linker_TransformMagicPortals(LinkerBspRenderGeometry *geometry, char *error, size_t errorSize);

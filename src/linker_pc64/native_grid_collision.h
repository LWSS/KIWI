#pragma once
#include <stddef.h>
struct clipMap_t;
struct LinkerGridCollision;
// Builds a world-only sight-query context from an imported, validated clipmap. The map remains borrowed.
bool Linker_CreateGridCollision(const clipMap_t *map, LinkerGridCollision **result, char *error, size_t errorSize);
void Linker_FreeGridCollision(LinkerGridCollision *context);
bool Linker_GridCollisionTrace(const float *start, const float *end, bool *visible, void *context);
// Nearest world point-trace hit, including the engine's 0.125-unit brush contact margin.
// A clear segment returns 1. The output is unchanged on invalid input.
bool Linker_GridCollisionFraction(const float *start, const float *end, float *fraction, void *context);

#pragma once
#include <stddef.h>
struct clipMap_t;
// Rebuild compiler-owned flat brush lists as spatial trees. Failure leaves the map unchanged.
bool Linker_SubdivideCollisionBrushes(clipMap_t *map, char *error, size_t errorSize);

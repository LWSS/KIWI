#pragma once
#include <stddef.h>
struct PhysGeomList;
// Reads iwmap 4 boxes, cylinders and convex brushes; computes unit-mass properties using the engine's voxel convention.
bool Linker_ImportPhysicsMap(const void *data, size_t size, PhysGeomList **map, char *error, size_t errorSize);
void Linker_FreePhysicsMap(PhysGeomList *map);

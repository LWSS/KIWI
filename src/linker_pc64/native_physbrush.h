#pragma once
#include <stddef.h>
struct BrushWrapper;
// Half-spaces use dot(normal, point) <= distance. Caller owns the complete returned brush.
bool Linker_BuildPhysicsBrush(const double (*planes)[4], unsigned int count, BrushWrapper **brush, char *error,
                              size_t errorSize);
void Linker_FreePhysicsBrush(BrushWrapper *brush);

#pragma once
#include <stddef.h>
struct PhysGeomList;
struct BrushWrapper;
bool DB64_ValidateBrush(const BrushWrapper *brush, bool children, char *error, size_t errorSize);
bool DB64_ValidatePhysicsGeometry(const PhysGeomList *list, char *error, size_t errorSize);
void DB64_LoadPhysicsGeometry(PhysGeomList **list);

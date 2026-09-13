#pragma once
#include <stddef.h>
struct PhysPreset;
// The returned preset owns its strings and is released with free().
bool Linker_ImportPhysicsPreset(const void *data, size_t size, const char *name, PhysPreset **result, char *error,
                                size_t errorSize);

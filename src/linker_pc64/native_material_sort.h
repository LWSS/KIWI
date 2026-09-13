#pragma once
#include <stddef.h>
#include <stdint.h>
struct Material;
// Produces renderer-compatible ranks without changing material drawSurf fields.
// NULL slots are allowed for unused BSP materials. Aliased pointers receive the same rank.
// The caller owns the result and releases it with free().
bool Linker_BuildMaterialRanks(Material *const *materials, unsigned int count, uint16_t **ranks, char *error,
                               size_t errorSize);

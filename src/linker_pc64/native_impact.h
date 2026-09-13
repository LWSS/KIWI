#pragma once
#include <stddef.h>
struct FxImpactTable;
struct FxEffectDef;
typedef const FxEffectDef *(*LinkerImpactEffect)(const char *name, void *context);
// Global fx/*.csv files are applied alphabetically, followed by fx/maps/<map>/*.csv.
// The returned table owns its entries; referenced effects remain callback-owned.
bool Linker_CompileImpactFiles(const char *root, const char *map, LinkerImpactEffect resolve, void *context,
                              FxImpactTable **result, char *error, size_t errorSize);

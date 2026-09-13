#pragma once
#include <stddef.h>
struct StringTable;
// Returned table owns all storage and is released with free().
bool Linker_ImportStringTable(const void *data, size_t size, const char *name, StringTable **result,
                              char *error, size_t errorSize);

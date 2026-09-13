#pragma once
#include <stddef.h>
struct Font_s;
struct Material;
// The font owns its glyphs/name; materials remain owned by the resolver. Free with free().
bool Linker_ImportFont(const void *data, size_t size, const char *name,
                       Material *(*resolve)(const char *, void *), void *context,
                       Font_s **result, char *error, size_t errorSize);

#pragma once
#include <stddef.h>
struct XAsset;
struct LinkerLocalization
{
    XAsset *assets;
    int count;
    char *text;
};
bool Linker_ImportLocalization(const void *data, size_t size, const char *name, const char *language,
                               LinkerLocalization **result, char *error, size_t errorSize);
void Linker_FreeLocalization(LinkerLocalization *source);

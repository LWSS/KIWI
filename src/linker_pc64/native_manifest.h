#pragma once
#include <stddef.h>
bool Linker_CompileNativeManifest(const char *root, const char *manifest, const char *output, bool compress,
                                  char *error, size_t errorSize, const char *level = "", const char *language = "english");
bool Linker_CompileNativeMap(const char *root, const char *bsp, const char *manifest, const char *output,
                             bool compress, char *error, size_t errorSize, const char *language = "english");

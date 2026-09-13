#pragma once
#include <stddef.h>
typedef bool (*LinkerScriptDependency)(const char *path, void *context);
typedef bool (*LinkerScriptAssetDependency)(const char *type, const char *name, void *context);
bool Linker_ScanScriptDependencies(const void *data, size_t size, LinkerScriptDependency add, void *context,
                                   char *error, size_t errorSize);
bool Linker_ScanScriptAssetDependencies(const void *data, size_t size, LinkerScriptDependency add,
                                        LinkerScriptAssetDependency addAsset, void *context,
                                        char *error, size_t errorSize);

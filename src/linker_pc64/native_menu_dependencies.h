#pragma once
#include <stddef.h>
struct MenuList;
typedef bool (*LinkerMenuDependency)(const char *type, const char *name, void *context);
bool Linker_VisitMenuScriptAssets(const MenuList *list, LinkerMenuDependency add, void *context,
                                 char *error, size_t errorSize);

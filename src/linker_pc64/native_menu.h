#pragma once
#include <stddef.h>
struct MenuList;
struct Material;
struct snd_alias_list_t;
struct LinkerCompiledMenu;
typedef Material *(*LinkerMenuMaterial)(const char *, void *);
typedef snd_alias_list_t *(*LinkerMenuSound)(const char *, void *);
bool Linker_CompileMenu(const char *root, const char *path, LinkerMenuMaterial material, LinkerMenuSound sound,
                        void *context, LinkerCompiledMenu **result, char *error, size_t errorSize);
MenuList *Linker_GetMenuList(LinkerCompiledMenu *menu);
void Linker_FreeMenu(LinkerCompiledMenu *menu);

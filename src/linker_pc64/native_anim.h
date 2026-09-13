#pragma once
#include <stddef.h>
struct ScriptStringList;
struct XAnimParts;
struct LinkerCompiledAnimation;
bool Linker_ImportAnimation(const void *data, size_t size, const char *name, ScriptStringList *strings,
                            LinkerCompiledAnimation **compiled, char *error, size_t errorSize);
XAnimParts *Linker_GetAnimation(LinkerCompiledAnimation *compiled);
void Linker_FreeAnimation(LinkerCompiledAnimation *compiled);

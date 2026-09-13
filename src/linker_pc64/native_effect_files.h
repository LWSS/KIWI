#pragma once
#include "native_fx_support.h"
struct LinkerEffectFiles;
LinkerEffectFiles *Linker_CreateEffectFiles(const char *root, LinkerFxServices dependencies);
const FxEffectDef *Linker_CompileEffectFile(LinkerEffectFiles *files, const char *name, char *error, size_t errorSize);
void Linker_FreeEffectFiles(LinkerEffectFiles *files);

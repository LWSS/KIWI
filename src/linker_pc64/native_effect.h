#pragma once
#include "native_fx_support.h"
struct LinkerCompiledEffect;
bool Linker_CompileEffectText(const void *data, size_t size, const char *name, LinkerFxServices dependencies,
                               LinkerCompiledEffect **effect, char *error, size_t errorSize);
const FxEffectDef *Linker_GetCompiledEffect(const LinkerCompiledEffect *effect);
void Linker_FreeCompiledEffect(LinkerCompiledEffect *effect);
// Visits sounds in this effect and all referenced effects, including cyclic graphs.
bool Linker_VisitEffectSounds(const FxEffectDef *effect, bool (*visit)(const char *, void *), void *context);

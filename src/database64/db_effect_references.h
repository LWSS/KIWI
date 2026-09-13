#pragma once
#include <stddef.h>

struct FxEffectDef;
union FxEffectDefRef;

// Effect-to-effect references are stored as names. Resolve after the complete
// zone is registered, so forward references and cycles never create defaults.
void DB64_DeferEffectReference(FxEffectDefRef *reference);
bool DB64_ResolveEffectReferences(const FxEffectDef *(*findLoaded)(const char *), char *error, size_t errorSize);
void DB64_ResetEffectReferences();
const FxEffectDef *DB64_FindLoadedEffect(const char *name);

#pragma once
#include <stddef.h>
struct FxEffectDef;
struct FxTrailDef;
struct FxElemDef;
bool DB64_EffectElementCount(const FxEffectDef *effect, size_t *count);
bool DB64_ValidateEffectElement(const FxElemDef *element);
bool DB64_ValidateTrail(const FxTrailDef *trail, bool checkIndices);
void DB64_LoadEffectTrail(FxTrailDef *trail, bool atStreamStart);
void DB64_LoadEffectSamples(FxElemDef *element);

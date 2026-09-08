#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

#include "qe3.h"

// 0x47D2A0 record: composed caster orientation then its brush pointer (52-byte x86 stride).
struct KiwiLightCasterRecord
{
    orientation_t orient;
    selbrush_t    *brush;
};
static_assert(sizeof(KiwiLightCasterRecord) == (sizeof(void *) == 8 ? 56 : 52), "light caster record");

typedef int (__cdecl *KiwiLightGatherFn)(KiwiLightCasterRecord *out,
                                         const float *origin, float radius);

// Keyed by light DEF, world origin, radius, and walk epoch; camera motion cannot change sphere intersections.
const KiwiLightCasterRecord *KiwiLightCache_Get(
    entity_s_def *lightDef, const float *origin, float radius,
    KiwiLightGatherFn gather, int *recordCount);

// Light epair transactions evict named lights and advance unrelated caster lists to the new epoch.
void KiwiLightCache_BeginLightEdit();
void KiwiLightCache_RetainUnaffected(entity_s_def *const *editedDefs, int editedCount);

// After common epair invalidation, ordinary light-key edits retain unrelated lists; other edits do not.
void KiwiLightCache_EntityKeyChanged(entity_s_def *lightDef, const char *key);

// Visibility does not bump the walk epoch; this generation rebuilds FilterBrush-pruned caster lists.
void KiwiLightCache_VisibilityChanged();

// Completes pending light-only retention after MarkMapModified's extra epoch bump.
void KiwiLightCache_MapModified();

void KiwiLightCache_Shutdown();

#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

#include "qe3.h"

// Raw CoD4Radiant LightPreview_GatherShadowBrushes record: the caster's composed
// orientation followed by its brush instance pointer.  The x86 layout is the
// binary's 52-byte stride.
struct KiwiLightCasterRecord
{
    orientation_t orient;
    selbrush_t    *brush;
};
static_assert(sizeof(KiwiLightCasterRecord) == 52, "light caster record");

typedef int (__cdecl *KiwiLightGatherFn)(KiwiLightCasterRecord *out,
                                         const float *origin, float radius);

// Key = (light entity DEF pointer, world origin, radius, shared walk-cache
// epoch).  Camera state is intentionally absent: moving the camera cannot change
// which world brushes intersect a light sphere.
const KiwiLightCasterRecord *KiwiLightCache_Get(
    entity_s_def *lightDef, const float *origin, float radius,
    KiwiLightGatherFn gather, int *recordCount);

// Called around a light epair transaction that bumps the shared epoch.  Those
// edits cannot change any other light's caster geometry, so only the named
// lights are evicted; unrelated records advance to the new epoch.
void KiwiLightCache_BeginLightEdit();
void KiwiLightCache_RetainUnaffected(entity_s_def *const *editedDefs, int editedCount);

// Post-KiwiShadowCache_Invalidate hook for the common epair funnel.  A normal
// light-key edit cannot change another light's caster set; structural and
// non-light edits keep the shared epoch's conservative full invalidation.
void KiwiLightCache_EntityKeyChanged(entity_s_def *lightDef, const char *key);

// Visibility is not map geometry, so the shared walk epoch intentionally does
// not move when a brush is hidden or shown.  Notify the per-light cache so its
// faithful FilterBrush-pruned caster list is rebuilt on the next preview.
void KiwiLightCache_VisibilityChanged();

// MarkMapModified adds a second shared-epoch bump after the common SetKeyValue
// funnel.  Finish a pending light-only transaction after that bump so unrelated
// per-light caster lists can advance instead of all being discarded.
void KiwiLightCache_MapModified();

void KiwiLightCache_Shutdown();

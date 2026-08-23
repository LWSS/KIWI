#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

#include "stdafx.h"
#include "qe3.h"
#include "kiwi_lightcache.h"
#include "kiwi_walkcache.h"

#include <stdlib.h>
#include <string.h>

namespace
{
    enum { KLIGHT_CACHE_SLOTS = 32, KLIGHT_MAX_CASTERS = 0x8000 };

    struct LightCacheEntry
    {
        entity_s_def          *def;
        float                  origin[3];
        float                  radius;
        unsigned               epoch;
        unsigned               stamp;
        KiwiLightCasterRecord *records;
        int                    count;
    };

    LightCacheEntry s_entries[KLIGHT_CACHE_SLOTS];
    unsigned        s_epoch = ~0u;
    unsigned        s_stamp = 0;
    int             s_filterGeneration = -1;
    int             s_editLayer = -1;
    unsigned        s_visibilityGeneration = 1;
    unsigned        s_seenVisibilityGeneration = 0;

    enum { KLIGHT_PENDING_EDITS = 64 };
    entity_s_def   *s_pendingEdits[KLIGHT_PENDING_EDITS];
    int             s_pendingEditCount = 0;
    bool            s_pendingOverflow = false;

    void ClearEntries()
    {
        for ( int i = 0; i < KLIGHT_CACHE_SLOTS; ++i )
        {
            free(s_entries[i].records);
            memset(&s_entries[i], 0, sizeof(s_entries[i]));
        }
    }

    bool SameFloat(float a, float b)
    {
        return memcmp(&a, &b, sizeof(float)) == 0;
    }

    bool Matches(const LightCacheEntry &e, entity_s_def *def,
                 const float *origin, float radius, unsigned epoch)
    {
        return e.def == def && e.epoch == epoch && SameFloat(e.radius, radius)
            && SameFloat(e.origin[0], origin[0])
            && SameFloat(e.origin[1], origin[1])
            && SameFloat(e.origin[2], origin[2]);
    }

    void ResetPendingEdits()
    {
        memset(s_pendingEdits, 0, sizeof(s_pendingEdits));
        s_pendingEditCount = 0;
        s_pendingOverflow = false;
    }

    void RememberPendingEdit(entity_s_def *def)
    {
        for ( int i = 0; i < s_pendingEditCount; ++i )
            if ( s_pendingEdits[i] == def )
                return;
        if ( s_pendingEditCount == KLIGHT_PENDING_EDITS )
        {
            s_pendingOverflow = true;
            return;
        }
        s_pendingEdits[s_pendingEditCount++] = def;
    }

    void SyncGenerations(unsigned epoch)
    {
        s_epoch = epoch;
        s_filterGeneration = g_qeglobals.g_filtersUpdated;
        s_editLayer = g_qeglobals.current_edit_layer;
        s_seenVisibilityGeneration = s_visibilityGeneration;
    }

    bool GenerationsMatch(unsigned epoch)
    {
        return s_epoch == epoch
            && s_filterGeneration == g_qeglobals.g_filtersUpdated
            && s_editLayer == g_qeglobals.current_edit_layer
            && s_seenVisibilityGeneration == s_visibilityGeneration;
    }

    void RetainUnaffectedImpl(entity_s_def *const *editedDefs, int editedCount,
                              unsigned epoch)
    {
        for ( int i = 0; i < KLIGHT_CACHE_SLOTS; ++i )
        {
            LightCacheEntry &e = s_entries[i];
            if ( !e.def )
                continue;
            bool edited = false;
            for ( int j = 0; j < editedCount; ++j )
                if ( e.def == editedDefs[j] ) { edited = true; break; }
            if ( edited )
            {
                free(e.records);
                memset(&e, 0, sizeof(e));
            }
            else
            {
                e.epoch = epoch;
            }
        }
        SyncGenerations(epoch);
    }
}

const KiwiLightCasterRecord *KiwiLightCache_Get(
    entity_s_def *lightDef, const float *origin, float radius,
    KiwiLightGatherFn gather, int *recordCount)
{
    if ( recordCount )
        *recordCount = 0;
    if ( !lightDef || !origin || !( radius > 0.0f ) || !gather )
        return nullptr;

    const unsigned epoch = KiwiWalkCache_Epoch();
    if ( !GenerationsMatch(epoch) )
    {
        ClearEntries();
        SyncGenerations(epoch);
        ResetPendingEdits();
    }

    for ( int i = 0; i < KLIGHT_CACHE_SLOTS; ++i )
    {
        LightCacheEntry &e = s_entries[i];
        if ( Matches(e, lightDef, origin, radius, epoch) )
        {
            e.stamp = ++s_stamp;
            if ( recordCount ) *recordCount = e.count;
            // A non-null stable address also represents a cached empty gather.
            return e.records ? e.records : (const KiwiLightCasterRecord *)&e;
        }
    }

    int victim = 0;
    for ( int i = 0; i < KLIGHT_CACHE_SLOTS; ++i )
    {
        if ( !s_entries[i].def ) { victim = i; break; }
        if ( s_entries[i].stamp < s_entries[victim].stamp ) victim = i;
    }

    KiwiLightCasterRecord *scratch = (KiwiLightCasterRecord *)
        malloc((size_t)KLIGHT_MAX_CASTERS * sizeof(KiwiLightCasterRecord));
    if ( !scratch )
        return nullptr;
    int count = gather(scratch, origin, radius);
    if ( count < 0 ) count = 0;
    if ( count > KLIGHT_MAX_CASTERS ) count = KLIGHT_MAX_CASTERS;

    KiwiLightCasterRecord *kept = scratch;
    if ( count == 0 )
    {
        free(scratch);
        kept = nullptr;
    }
    else
    {
        void *shrunk = realloc(scratch, (size_t)count * sizeof(KiwiLightCasterRecord));
        if ( shrunk ) kept = (KiwiLightCasterRecord *)shrunk;
    }

    LightCacheEntry &e = s_entries[victim];
    free(e.records);
    memset(&e, 0, sizeof(e));
    e.def = lightDef;
    memcpy(e.origin, origin, sizeof(e.origin));
    e.radius = radius;
    e.epoch = epoch;
    e.stamp = ++s_stamp;
    e.records = kept;
    e.count = count;
    if ( recordCount ) *recordCount = count;
    return kept ? kept : (const KiwiLightCasterRecord *)&e;
}

void KiwiLightCache_BeginLightEdit()
{
    const unsigned epoch = KiwiWalkCache_Epoch();
    if ( !GenerationsMatch(epoch) )
    {
        ClearEntries();
        SyncGenerations(epoch);
    }
    ResetPendingEdits();
}

void KiwiLightCache_RetainUnaffected(entity_s_def *const *editedDefs, int editedCount)
{
    if ( !editedDefs || editedCount <= 0 )
        return;

    RetainUnaffectedImpl(editedDefs, editedCount, KiwiWalkCache_Epoch());
    ResetPendingEdits();
}

void KiwiLightCache_EntityKeyChanged(entity_s_def *lightDef, const char *key)
{
    if ( !lightDef || !lightDef->eclass || !( lightDef->eclass->classtype & 1 )
      || !key || !_stricmp(key, "classname") )
        return;

    const unsigned epoch = KiwiWalkCache_Epoch();
    if ( s_epoch + 1u != epoch )
    {
        // Another invalidation happened since these records were current; do not
        // revive stale caster lists merely because the latest edit was a light.
        ClearEntries();
        SyncGenerations(epoch);
        ResetPendingEdits();
    }
    else
    {
        entity_s_def *edited = lightDef;
        RetainUnaffectedImpl(&edited, 1, epoch);
    }
    RememberPendingEdit(lightDef);
}

void KiwiLightCache_VisibilityChanged()
{
    ++s_visibilityGeneration;
}

void KiwiLightCache_MapModified()
{
    const unsigned epoch = KiwiWalkCache_Epoch();
    if ( s_pendingEditCount > 0 && !s_pendingOverflow && s_epoch + 1u == epoch )
        RetainUnaffectedImpl(s_pendingEdits, s_pendingEditCount, epoch);
    ResetPendingEdits();
}

void KiwiLightCache_Shutdown()
{
    ClearEntries();
    s_epoch = ~0u;
    s_stamp = 0;
    s_filterGeneration = -1;
    s_editLayer = -1;
    s_seenVisibilityGeneration = 0;
    ResetPendingEdits();
}

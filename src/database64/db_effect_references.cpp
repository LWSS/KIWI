#include <universal/q_shared.h>
#include <gfx_d3d/fxprimitives.h>
#include "database.h"
#include "db_effect_references.h"
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>

struct EffectReferenceFixup
{
    FxEffectDefRef *destination;
    const char *name;
    const FxEffectDef *resolved;
};

static EffectReferenceFixup *s_fixups;
static size_t s_count;
static size_t s_capacity;

void DB64_ResetEffectReferences()
{
    free(s_fixups);
    s_fixups = NULL;
    s_count = 0;
    s_capacity = 0;
}

void DB64_DeferEffectReference(FxEffectDefRef *reference)
{
    if (!reference->name)
    {
        return;
    }
    if (s_count == s_capacity)
    {
        const size_t capacity = s_capacity ? s_capacity * 2 : 64;
        if (capacity > INT_MAX / sizeof(EffectReferenceFixup))
        {
            DB64_ResetEffectReferences();
            Com_Error(ERR_DROP, "Too many native effect references");
        }
        EffectReferenceFixup *next = (EffectReferenceFixup *)realloc(s_fixups, capacity * sizeof(EffectReferenceFixup));
        if (!next)
        {
            DB64_ResetEffectReferences();
            Com_Error(ERR_DROP, "Out of memory resolving native effects");
        }
        s_fixups = next;
        s_capacity = capacity;
    }
    s_fixups[s_count].destination = reference;
    s_fixups[s_count].name = reference->name;
    s_fixups[s_count].resolved = NULL;
    ++s_count;
}

bool DB64_ResolveEffectReferences(const FxEffectDef *(*findLoaded)(const char *), char *error, size_t errorSize)
{
    if (errorSize)
    {
        error[0] = 0;
    }
    for (size_t i = 0; i < s_count; ++i)
    {
        s_fixups[i].resolved = findLoaded(s_fixups[i].name);
        if (!s_fixups[i].resolved)
        {
            if (errorSize)
            {
                snprintf(error, errorSize, "Native effect dependency '%s' is not loaded", s_fixups[i].name);
            }
            DB64_ResetEffectReferences();
            return false;
        }
    }
    // Publish only after every dependency exists. No recursive graph walk is
    // needed: every pointer is patched exactly once, including back edges.
    for (size_t i = 0; i < s_count; ++i)
    {
        s_fixups[i].destination->handle = s_fixups[i].resolved;
    }
    DB64_ResetEffectReferences();
    return true;
}

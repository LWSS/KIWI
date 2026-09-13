#include <universal/q_shared.h>
#include <universal/q_parse.h>
#include <EffectsCore/fx_system.h>
#include <database64/db_effect_layout.h>
#include "native_effect.h"
#include <stdio.h>
#include <stdlib.h>

struct EffectAllocation
{
    EffectAllocation *next;
    void *data;
};
struct LinkerCompiledEffect
{
    EffectAllocation *allocations;
    const FxEffectDef *effect;
    LinkerFxServices dependencies;
};
static thread_local LinkerCompiledEffect *s_current;

static bool AddSoundEffect(const FxEffectDef *effect, const FxEffectDef **effects, int *count)
{
    if (!effect)
    {
        return true;
    }
    for (int i = 0; i < *count; ++i)
    {
        if (effects[i] == effect)
        {
            return true;
        }
    }
    if (*count == 4096)
    {
        return false;
    }
    effects[(*count)++] = effect;
    return true;
}

bool Linker_VisitEffectSounds(const FxEffectDef *effect, bool (*visit)(const char *, void *), void *context)
{
    if (!visit)
    {
        return false;
    }
    const FxEffectDef *effects[4096];
    int count = 0;
    AddSoundEffect(effect, effects, &count);
    for (int i = 0; i < count; ++i)
    {
        size_t elementCount;
        if (!DB64_EffectElementCount(effects[i], &elementCount))
        {
            return false;
        }
        for (size_t j = 0; j < elementCount; ++j)
        {
            const FxElemDef *element = &effects[i]->elemDefs[j];
            if (!DB64_ValidateEffectElement(element) ||
                !AddSoundEffect(element->effectOnImpact.handle, effects, &count) ||
                !AddSoundEffect(element->effectOnDeath.handle, effects, &count) ||
                !AddSoundEffect(element->effectEmitted.handle, effects, &count))
            {
                return false;
            }
            if (element->elemType != 8 && element->elemType != 10)
            {
                continue;
            }
            for (int k = 0; k < element->visualCount; ++k)
            {
                const FxElemVisuals *visual = element->visualCount > 1 ?
                    &element->visuals.array[k] : &element->visuals.instance;
                if (element->elemType == 10)
                {
                    if (!AddSoundEffect(visual->effectDef.handle, effects, &count))
                    {
                        return false;
                    }
                }
                else if (visual->soundName && visual->soundName[0] && !visit(visual->soundName, context))
                {
                    return false;
                }
            }
        }
    }
    return true;
}

void Linker_FreeCompiledEffect(LinkerCompiledEffect *effect)
{
    if (effect)
    {
        while (effect->allocations)
        {
            EffectAllocation *next = effect->allocations->next;
            _aligned_free(effect->allocations->data);
            free(effect->allocations);
            effect->allocations = next;
        }
        free(effect);
    }
}
const FxEffectDef *Linker_GetCompiledEffect(const LinkerCompiledEffect *effect)
{
    return effect ? effect->effect : NULL;
}
static void *Allocate(size_t size, size_t alignment, void *context)
{
    LinkerCompiledEffect *owner = (LinkerCompiledEffect *)context;
    EffectAllocation *allocation = (EffectAllocation *)malloc(sizeof(EffectAllocation));
    if (!allocation)
    {
        return NULL;
    }
    allocation->data = _aligned_malloc(size ? size : 1, alignment < sizeof(void *) ? sizeof(void *) : alignment);
    if (!allocation->data)
    {
        free(allocation);
        return NULL;
    }
    memset(allocation->data, 0, size);
    allocation->next = owner->allocations;
    owner->allocations = allocation;
    return allocation->data;
}
static void *AllocateRuntime(uint size)
{
    return Allocate(size, 16, s_current);
}
static PhysPreset *Preset(const char *name, void *context)
{
    const LinkerFxServices *s = &((LinkerCompiledEffect *)context)->dependencies;
    return s->preset ? s->preset(name, s->context) : NULL;
}
static Material *MaterialAsset(const char *name, void *context)
{
    const LinkerFxServices *s = &((LinkerCompiledEffect *)context)->dependencies;
    return s->material ? s->material(name, s->context) : NULL;
}
static XModel *Model(const char *name, void *context)
{
    const LinkerFxServices *s = &((LinkerCompiledEffect *)context)->dependencies;
    return s->model ? s->model(name, s->context) : NULL;
}
static const FxEffectDef *Effect(const char *name, void *context)
{
    const LinkerFxServices *s = &((LinkerCompiledEffect *)context)->dependencies;
    return s->effect ? s->effect(name, s->context) : NULL;
}

static bool HasCurves(const FxEditorEffectDef *effect)
{
    for (int i = 0; i < effect->elemCount; ++i)
    {
        const FxEditorElemDef *e = &effect->elems[i];
        for (int variant = 0; variant < 2; ++variant)
        {
            if (!e->rotationShape[variant] || !e->scaleShape[variant] || !e->color[variant] || !e->alpha[variant])
            {
                return false;
            }
            for (int frame = 0; frame < 2; ++frame)
            {
                if (!e->sizeShape[frame][variant])
                {
                    return false;
                }
                for (int axis = 0; axis < 3; ++axis)
                {
                    if (!e->velShape[frame][axis][variant])
                    {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

bool Linker_CompileEffectText(const void *data, size_t size, const char *name, LinkerFxServices dependencies,
                              LinkerCompiledEffect **effect, char *error, size_t errorSize)
{
    if (!effect || (!error && errorSize))
    {
        return false;
    }
    *effect = NULL;
    if (!data || size > 16 * 1024 * 1024 || !name || !*name || strlen(name) >= 64 || memchr(data, 0, size))
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid FX source text or name");
        }
        return false;
    }
    LinkerCompiledEffect *result = (LinkerCompiledEffect *)calloc(1, sizeof(LinkerCompiledEffect));
    if (!result)
    {
        return false;
    }
    result->dependencies = dependencies;
    char *text = (char *)Allocate(size + 1, 1, result);
    FxEditorEffectDef *editor = (FxEditorEffectDef *)Allocate(sizeof(FxEditorEffectDef), 16, result);
    ParseThreadInfo *saved = (ParseThreadInfo *)malloc(sizeof(ParseThreadInfo));
    if (!text || !editor || !saved)
    {
        free(saved);
        Linker_FreeCompiledEffect(result);
        return false;
    }
    memcpy(text, data, size);
    strcpy(editor->name, name);
    *saved = *Com_GetParseThreadInfo();
    const LinkerFxServices services = {result, Allocate, Preset, MaterialAsset, Model, Effect};
    const LinkerFxServices previous = Linker_SetFxServices(services);
    LinkerCompiledEffect *parent = s_current;
    s_current = result;
    bool valid = false;
    try
    {
        valid = FX_LoadEditorEffectFromBuffer(text, name, editor) && HasCurves(editor);
        if (valid)
        {
            result->effect = FX_Convert(editor, AllocateRuntime);
            valid = result->effect != NULL;
            if (valid && !result->effect->elemDefCountLooping && !result->effect->elemDefCountOneShot &&
                !result->effect->elemDefCountEmission)
            {
                ((FxEffectDef *)result->effect)->elemDefs = NULL;
            }
        }
    }
    catch (...)
    {
        valid = false;
    }
    s_current = parent;
    Linker_SetFxServices(previous);
    *Com_GetParseThreadInfo() = *saved;
    free(saved);
    if (!valid)
    {
        Linker_FreeCompiledEffect(result);
        if (errorSize)
        {
            snprintf(error, errorSize, "FX parse/conversion failed or required curves are missing: %s", name);
        }
        return false;
    }
    *effect = result;
    return true;
}

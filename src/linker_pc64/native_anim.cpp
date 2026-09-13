#include <universal/q_shared.h>
#include <xanim/xanim.h>
#include <database64/database.h>
#include "native_anim.h"
#include "native_anim_source.h"
#include "native_model_files.h"
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>

struct AnimationAllocation
{
    AnimationAllocation *next;
    void *data;
};
struct LinkerCompiledAnimation
{
    AnimationAllocation *allocations;
    XAnimParts *parts;
};
struct AnimationCompileContext
{
    LinkerCompiledAnimation *compiled;
    const void *source;
    size_t size;
    ScriptStringList *strings;
};
static thread_local AnimationCompileContext *s_animationContext;
extern HunkUser *g_animUser;
extern XAnimParts *XAnimLoadFile(char *name, void *(*allocate)(int));

static void *AllocateAnimation(int size)
{
    if (!s_animationContext || size <= 0)
    {
        throw 1;
    }
    AnimationAllocation *allocation = (AnimationAllocation *)calloc(1, sizeof(AnimationAllocation));
    if (!allocation)
    {
        throw 1;
    }
    allocation->data = calloc(1, (size_t)size);
    if (!allocation->data)
    {
        free(allocation);
        throw 1;
    }
    allocation->next = s_animationContext->compiled->allocations;
    s_animationContext->compiled->allocations = allocation;
    return allocation->data;
}

int LinkerAnimReadFile(const char *, void **data)
{
    *data = (void *)s_animationContext->source;
    return (int)s_animationContext->size;
}
void LinkerAnimFreeFile(void *) {}
unsigned int LinkerAnimString(const char *text, int, int)
{
    const uint16_t index = Linker_InternModelString(text, s_animationContext->strings);
    if (!index)
    {
        throw 1;
    }
    return index;
}
unsigned int LinkerAnimStringSize(const char *text, int user, unsigned int, int type)
{
    return LinkerAnimString(text, user, type);
}
HunkUser *LinkerAnimCreate(int, const char *, int, int, int)
{
    return (HunkUser *)s_animationContext;
}
void *LinkerAnimAllocate(HunkUser *, int size, int)
{
    return AllocateAnimation(size);
}
void LinkerAnimDestroy(HunkUser *) {}

void Linker_FreeAnimation(LinkerCompiledAnimation *compiled)
{
    if (!compiled)
    {
        return;
    }
    AnimationAllocation *allocation = compiled->allocations;
    while (allocation)
    {
        AnimationAllocation *next = allocation->next;
        free(allocation->data);
        free(allocation);
        allocation = next;
    }
    free(compiled);
}

XAnimParts *Linker_GetAnimation(LinkerCompiledAnimation *compiled)
{
    return compiled ? compiled->parts : NULL;
}

bool Linker_ImportAnimation(const void *data, size_t size, const char *name, ScriptStringList *strings,
                            LinkerCompiledAnimation **compiled, char *error, size_t errorSize)
{
    *compiled = NULL;
    if (!name || !name[0] || strlen(name) >= 58 || !strings || !strings->strings ||
        strings->count < 1 || strings->count > 65536 || size > INT_MAX || s_animationContext)
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid animation compiler input");
        }
        return false;
    }
    if (!Linker_ValidateAnimationSource(data, size, error, errorSize))
    {
        return false;
    }
    LinkerCompiledAnimation *result = (LinkerCompiledAnimation *)calloc(1, sizeof(LinkerCompiledAnimation));
    if (!result)
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Out of memory compiling animation");
        }
        return false;
    }
    AnimationCompileContext context = {result, data, size, strings};
    s_animationContext = &context;
    try
    {
        char *ownedName = (char *)AllocateAnimation((int)strlen(name) + 1);
        strcpy(ownedName, name);
        result->parts = XAnimLoadFile(ownedName, AllocateAnimation);
        if (result->parts)
        {
            result->parts->name = ownedName;
        }
    }
    catch (...)
    {
        result->parts = NULL;
    }
    g_animUser = NULL;
    s_animationContext = NULL;
    if (!result->parts)
    {
        Linker_FreeAnimation(result);
        if (errorSize)
        {
            snprintf(error, errorSize, "Animation conversion failed: %s", name);
        }
        return false;
    }
    *compiled = result;
    return true;
}

#include <universal/q_shared.h>
#include "ui_shared.h"
#include <universal/com_memory.h>

void __cdecl UILocalVar_Init(UILocalVarContext *context)
{
    iassert(context);
}

void __cdecl UILocalVar_Shutdown(UILocalVarContext *context)
{
    uint hash; // [esp+4h] [ebp-4h]

    iassert(context);
    for (hash = 0; hash < 0x100; ++hash)
    {
        if (context->table[hash].name)
        {
            if (context->table[hash].type == UILOCALVAR_STRING)
                FreeString(context->table[hash].u.string);
            FreeString(context->table[hash].name);
        }
    }
    memset((uint8_t *)context, 0, sizeof(UILocalVarContext));
}

UILocalVarContext *__cdecl UILocalVar_Find(UILocalVarContext *context, const char *name)
{
    uint hash; // [esp+0h] [ebp-4h] BYREF

    if (UILocalVar_FindLocation(context, name, &hash))
        return (UILocalVarContext *)((char *)context + 12 * hash);
    else
        return 0;
}

char __cdecl UILocalVar_FindLocation(UILocalVarContext *context, const char *name, uint *hashForName)
{
    uint hash; // [esp+1Ch] [ebp-8h]
    uint initialHash; // [esp+20h] [ebp-4h]

    initialHash = UILocalVar_HashName(name);
    hash = initialHash;
    do
    {
        if (!context->table[hash].name)
            break;
        if (!strcmp(context->table[hash].name, name))
        {
            *hashForName = hash;
            return 1;
        }
        hash = (uint8_t)(hash + 1);
    } while (hash != initialHash);
    *hashForName = hash;
    return 0;
}

uint __cdecl UILocalVar_HashName(const char *name)
{
    __int16 hash; // [esp+0h] [ebp-8h]
    uint i; // [esp+4h] [ebp-4h]

    hash = 0;
    for (i = 0; name[i]; ++i)
        hash += (i + 119) * name[i];
    return (uint8_t)(hash + HIBYTE(hash));
}

UILocalVarContext *__cdecl UILocalVar_FindOrCreate(UILocalVarContext *context, char *name)
{
    UILocalVar *var; // [esp+0h] [ebp-8h]
    uint hash; // [esp+4h] [ebp-4h] BYREF

    if (UILocalVar_FindLocation(context, name, &hash))
        return (UILocalVarContext *)((char *)context + 12 * hash);
    var = &context->table[hash];
    var->name = CopyString(name);
    var->type = UILOCALVAR_INT;
    var->u.integer = 0;
    return (UILocalVarContext *)var;
}

bool __cdecl UILocalVar_GetBool(const UILocalVar *var)
{
    iassert(var);
    if (var->type == UILOCALVAR_INT)
        return var->u.integer != 0;
    if (var->type == UILOCALVAR_FLOAT)
        return var->u.value != 0.0;
    vassert(var->type == UILOCALVAR_STRING, "%i, %i", var->type, 2);
    return atoi(var->u.string) != 0;
}

UILocalVar_u __cdecl UILocalVar_GetInt(const UILocalVar *var)
{
    iassert(var);
    if (var->type)
    {
        if (var->type == UILOCALVAR_FLOAT)
        {
            return (UILocalVar_u)(int)var->u.value;
        }
        else
        {
            vassert(var->type == UILOCALVAR_STRING, "%i, %i", var->type, 2);
            return (UILocalVar_u)atoi(var->u.string);
        }
    }
    else
    {
        return var->u;
    }
}

double __cdecl UILocalVar_GetFloat(const UILocalVar *var)
{
    iassert(var);
    if (var->type == UILOCALVAR_INT)
        return (double)var->u.integer;
    if (var->type == UILOCALVAR_FLOAT)
        return var->u.value;
    vassert(var->type == UILOCALVAR_STRING, "%i, %i", var->type, 2);
    return (float)atof(var->u.string);
}

char *__cdecl UILocalVar_GetString(const UILocalVar *var, char *stringBuf, uint size)
{
    iassert(var);
    if (var->type)
    {
        if (var->type == UILOCALVAR_FLOAT)
        {
            Com_sprintf(stringBuf, size, "%g", var->u.value);
            return stringBuf;
        }
        else
        {
            vassert(var->type == UILOCALVAR_STRING, "%i, %i", var->type, 2);
            return (char *)var->u.integer;
        }
    }
    else
    {
        Com_sprintf(stringBuf, size, "%i", var->u.integer);
        return stringBuf;
    }
}

void __cdecl UILocalVar_SetBool(UILocalVar *var, bool b)
{
    if (var->type == UILOCALVAR_STRING)
        FreeString(var->u.string);
    var->type = UILOCALVAR_INT;
    var->u.integer = b;
}

void __cdecl UILocalVar_SetInt(UILocalVar *var, int i)
{
    if (var->type == UILOCALVAR_STRING)
        FreeString(var->u.string);
    var->type = UILOCALVAR_INT;
    var->u.integer = i;
}

void __cdecl UILocalVar_SetFloat(UILocalVar *var, float f)
{
    if (var->type == UILOCALVAR_STRING)
        FreeString(var->u.string);
    var->type = UILOCALVAR_FLOAT;
    var->u.value = f;
}

void __cdecl UILocalVar_SetString(UILocalVar *var, char *s)
{
    if (var->type == UILOCALVAR_STRING)
        FreeString(var->u.string);
    var->type = UILOCALVAR_STRING;
    var->u.integer = (int)CopyString(s);
}


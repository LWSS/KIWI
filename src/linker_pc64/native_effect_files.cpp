#include <universal/q_shared.h>
#include <database64/db_package.h>
#include "native_effect_files.h"
#include "native_effect.h"
#include "native_asset_files.h"
#include <stdio.h>
#include <stdlib.h>

struct EffectFileEntry
{
    char name[64];
    LinkerCompiledEffect *compiled;
    bool loading;
};
struct LinkerEffectFiles
{
    char *root;
    LinkerFxServices dependencies;
    EffectFileEntry *entries;
    unsigned int count, depth;
    char *error;
    size_t errorSize;
    bool failed;
};

LinkerEffectFiles *Linker_CreateEffectFiles(const char *root, LinkerFxServices dependencies)
{
    if (!root)
    {
        return NULL;
    }
    LinkerEffectFiles *result = (LinkerEffectFiles *)calloc(1, sizeof(LinkerEffectFiles));
    if (result)
    {
        result->root = _strdup(root);
        result->dependencies = dependencies;
        if (!result->root)
        {
            free(result);
            return NULL;
        }
    }
    return result;
}
void Linker_FreeEffectFiles(LinkerEffectFiles *files)
{
    if (files)
    {
        for (unsigned int i = 0; i < files->count; ++i)
        {
            Linker_FreeCompiledEffect(files->entries[i].compiled);
        }
        free(files->entries);
        free(files->root);
        free(files);
    }
}
static const FxEffectDef *LoadEffect(const char *name, void *context);
static Material *LoadMaterial(const char *name, void *context)
{
    LinkerEffectFiles *files = (LinkerEffectFiles *)context;
    Material *result =
        files->dependencies.material ? files->dependencies.material(name, files->dependencies.context) : NULL;
    files->failed = files->failed || !result;
    return result;
}
static XModel *LoadModel(const char *name, void *context)
{
    LinkerEffectFiles *files = (LinkerEffectFiles *)context;
    XModel *result = files->dependencies.model ? files->dependencies.model(name, files->dependencies.context) : NULL;
    files->failed = files->failed || !result;
    return result;
}
static PhysPreset *LoadPreset(const char *name, void *context)
{
    LinkerEffectFiles *files = (LinkerEffectFiles *)context;
    PhysPreset *result =
        files->dependencies.preset ? files->dependencies.preset(name, files->dependencies.context) : NULL;
    files->failed = files->failed || !result;
    return result;
}
static const FxEffectDef *Failure(LinkerEffectFiles *files, const char *name, const char *reason)
{
    files->failed = true;
    if (files->errorSize)
    {
        snprintf(files->error, files->errorSize, "FX %s: %s", name ? name : "<null>", reason);
    }
    return NULL;
}
static const FxEffectDef *LoadEffect(const char *name, void *context)
{
    LinkerEffectFiles *files = (LinkerEffectFiles *)context;
    char normalized[64];
    if (!name || !DB64_NormalizePath(name, normalized, sizeof(normalized)))
    {
        return Failure(files, name, "invalid asset name");
    }
    for (unsigned int i = 0; i < files->count; ++i)
    {
        if (!_stricmp(normalized, files->entries[i].name))
        {
            if (files->entries[i].loading)
            {
                return Failure(files, name, "recursive conversion dependency");
            }
            return files->entries[i].compiled ? Linker_GetCompiledEffect(files->entries[i].compiled)
                                              : Failure(files, name, "previous compilation failed");
        }
    }
    if (files->count == 4096 || files->depth == 32)
    {
        return Failure(files, name, "dependency limit exceeded");
    }
    EffectFileEntry *next = (EffectFileEntry *)realloc(files->entries, (files->count + 1) * sizeof(EffectFileEntry));
    if (!next)
    {
        return Failure(files, name, "allocation failed");
    }
    files->entries = next;
    const unsigned int index = files->count++;
    files->entries[index] = {};
    strcpy(files->entries[index].name, normalized);
    files->entries[index].loading = true;
    char path[80];
    snprintf(path, sizeof(path), "fx/%s.efx", normalized);
    void *bytes = NULL;
    size_t size = 0;
    bool valid = Linker_ReadRawAssetFile(files->root, path, &bytes, &size, 16 * 1024 * 1024);
    LinkerCompiledEffect *compiled = NULL;
    if (valid)
    {
        const LinkerFxServices services = {files, NULL, LoadPreset, LoadMaterial, LoadModel, LoadEffect};
        ++files->depth;
        valid = Linker_CompileEffectText(bytes, size, normalized, services, &compiled, files->error, files->errorSize);
        --files->depth;
    }
    free(bytes);
    files->entries[index].loading = false;
    if (!valid || files->failed)
    {
        Linker_FreeCompiledEffect(compiled);
        return Failure(files, normalized, "source or dependency compilation failed");
    }
    files->entries[index].compiled = compiled;
    return Linker_GetCompiledEffect(compiled);
}
const FxEffectDef *Linker_CompileEffectFile(LinkerEffectFiles *files, const char *name, char *error, size_t errorSize)
{
    if (!files || files->depth || (!error && errorSize))
    {
        return NULL;
    }
    files->error = error;
    files->errorSize = errorSize;
    files->failed = false;
    if (errorSize)
    {
        error[0] = 0;
    }
    const FxEffectDef *result = LoadEffect(name, files);
    files->error = NULL;
    files->errorSize = 0;
    return result;
}

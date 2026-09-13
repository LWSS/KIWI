#include <universal/q_shared.h>
#include <xanim/xmodel.h>
#include <xanim/xanim.h>
#include <database64/db_package.h>
#include "native_model_files.h"
#include "native_model.h"
#include "native_material_files.h"
#include "native_physics.h"
#include "native_physmap.h"
#include "native_asset_files.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint16_t Linker_InternModelString(const char *text, void *context)
{
    ScriptStringList *strings = (ScriptStringList *)context;
    if (!text || !strings || !strings->strings || strings->count < 1 || strings->count > 65536)
    {
        return 0;
    }
    for (int i = 1; i < strings->count; ++i)
    {
        if (!strcmp(strings->strings[i], text))
        {
            return (uint16_t)i;
        }
    }
    if (strings->count == 65536)
    {
        return 0;
    }
    char *copy = _strdup(text);
    if (!copy)
    {
        return 0;
    }
    strings->strings[strings->count] = copy;
    return (uint16_t)strings->count++;
}

static bool ReadModelDependency(const char *root, const char *path, void **data, size_t *size, size_t limit,
                                char *error, size_t errorSize)
{
    if (Linker_ReadRawAssetFile(root, path, data, size, limit))
    {
        return true;
    }
    if (errorSize)
    {
        snprintf(error, errorSize, "Cannot read model dependency '%s'", path);
    }
    return false;
}

void Linker_FreeCompiledModel(LinkerCompiledModel *compiled)
{
    if (!compiled)
    {
        return;
    }
    Linker_FreeAssembledModel(compiled->model);
    Linker_FreeModelSurfaces(compiled->surfaces, compiled->surfaceCount);
    Linker_FreeModelSource(compiled->source);
    for (unsigned int i = 0; i < compiled->materialCount; ++i)
    {
        Linker_FreeCompiledMaterial(compiled->ownedMaterials[i]);
    }
    free(compiled->preset);
    Linker_FreePhysicsMap(compiled->physics);
    free(compiled);
}

bool Linker_CompileModelFiles(const char *root, const char *name, LinkerInternModelString intern, void *context,
                              LinkerCompiledModel **compiled, char *error, size_t errorSize)
{
    if (!compiled || (!error && errorSize))
    {
        return false;
    }
    *compiled = NULL;
    if (errorSize)
    {
        error[0] = 0;
    }
    char normalized[1024], relative[2048];
    if (!root || !name || !intern || !DB64_NormalizePath(name, normalized, sizeof(normalized)))
    {
        return false;
    }
    snprintf(relative, sizeof(relative), "xmodel/%s", normalized);
    void *data = NULL, *parts = NULL;
    size_t size = 0, partsSize = 0, consumed = 0;
    LinkerModelConfig config = {};
    bool valid = ReadModelDependency(root, relative, &data, &size, 16 * 1024 * 1024, error, errorSize) &&
                 Linker_ReadModelConfig(data, size, &config, &consumed, error, errorSize);
    if (valid)
    {
        snprintf(relative, sizeof(relative), "xmodelparts/%s", config.lods[0].filename);
        valid = ReadModelDependency(root, relative, &parts, &partsSize, 16 * 1024 * 1024, error, errorSize);
    }
    LinkerCompiledModel *result = valid ? (LinkerCompiledModel *)calloc(1, sizeof(LinkerCompiledModel)) : NULL;
    valid = valid && result && Linker_ReadModelSource(data, size, parts, partsSize, &result->source, error, errorSize);
    free(data);
    free(parts);
    data = NULL;
    if (valid)
    {
        result->surfaceCount = result->source->metadata->surfaceCount;
        result->surfaces = (XSurface *)calloc(result->surfaceCount, sizeof(XSurface));
        valid = result->surfaces != NULL;
    }
    unsigned int first = 0;
    for (unsigned int lod = 0; valid && lod < 4; ++lod)
    {
        if (!config.lods[lod].filename[0])
        {
            continue;
        }
        snprintf(relative, sizeof(relative), "xmodelsurfs/%s", config.lods[lod].filename);
        LinkerModelMesh *mesh = NULL;
        XSurface *surfaces = NULL;
        valid = ReadModelDependency(root, relative, &data, &size, 256 * 1024 * 1024, error, errorSize) &&
                Linker_ReadModelMesh(data, size, result->source->skeleton->boneCount, &mesh, error, errorSize);
        free(data);
        data = NULL;
        if (valid && (mesh->surfaceCount != result->source->metadata->lodSurfaceCounts[lod] ||
                      mesh->surfaceCount > result->surfaceCount - first))
        {
            if (errorSize)
            {
                snprintf(error, errorSize, "LOD %u mesh '%s' has %u surfaces; model metadata expects %u", lod,
                         config.lods[lod].filename, mesh->surfaceCount,
                         result->source->metadata->lodSurfaceCounts[lod]);
            }
            valid = false;
        }
        valid =
            valid && Linker_BuildModelSurfaces(mesh, result->source->skeleton->boneCount, &surfaces, error, errorSize);
        if (valid)
        {
            memcpy(result->surfaces + first, surfaces, mesh->surfaceCount * sizeof(XSurface));
            first += mesh->surfaceCount;
            // Move the nested allocations into the combined surface table.
            free(surfaces);
        }
        Linker_FreeModelMesh(mesh);
    }
    for (unsigned int i = 0; valid && i < result->surfaceCount; ++i)
    {
        const char *materialName = result->source->metadata->materials[i];
        for (unsigned int j = 0; j < i; ++j)
        {
            if (!strcmp(materialName, result->source->metadata->materials[j]))
            {
                result->materials[i] = result->materials[j];
                break;
            }
        }
        if (!result->materials[i])
        {
            LinkerCompiledMaterial *material = NULL;
            valid = Linker_CompileMaterialFiles(root, materialName, IMAGE_TRACK_MODEL, &material, error, errorSize);
            if (!valid && errorSize && !error[0])
            {
                snprintf(error, errorSize, "Cannot compile model material '%s'", materialName);
            }
            if (valid)
            {
                result->ownedMaterials[result->materialCount++] = material;
                result->materials[i] = material->material;
            }
        }
    }
    if (valid && config.physicsPreset[0])
    {
        snprintf(relative, sizeof(relative), "physic/%s", config.physicsPreset);
        valid = ReadModelDependency(root, relative, &data, &size, 16 * 1024 * 1024, error, errorSize) &&
                Linker_ImportPhysicsPreset(data, size, config.physicsPreset, &result->preset, error, errorSize);
        free(data);
        data = NULL;
    }
    if (valid)
    {
        snprintf(relative, sizeof(relative), "phys_collmaps/%s.map", normalized);
        bool found = false;
        const bool read = Linker_ReadRawAssetFile(root, relative, &data, &size, 16 * 1024 * 1024, &found);
        if (found)
        {
            valid = read && Linker_ImportPhysicsMap(data, size, &result->physics, error, errorSize);
        }
        free(data);
        data = NULL;
    }
    uint16_t boneNames[128] = {};
    for (unsigned int i = 0; valid && i < result->source->skeleton->boneCount; ++i)
    {
        boneNames[i] = intern(result->source->skeleton->names[i], context);
        valid = boneNames[i] != 0;
        if (!valid && errorSize)
        {
            snprintf(error, errorSize, "Cannot intern bone %u named '%s'", i, result->source->skeleton->names[i]);
        }
    }
    valid = valid &&
            Linker_AssembleModel(normalized, result->source, result->surfaces, result->surfaceCount, result->materials,
                                 boneNames, result->preset, result->physics, &result->model, error, errorSize);
    if (!valid)
    {
        Linker_FreeCompiledModel(result);
        if (errorSize)
        {
            char detail[512];
            snprintf(detail, sizeof(detail), "%s", error[0] ? error : "Cannot read or compile model dependency");
            snprintf(error, errorSize, "%s: %s", normalized, detail);
        }
        return false;
    }
    *compiled = result;
    return true;
}

#include <universal/q_shared.h>
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/r_image.h>
#include <gfx_d3d/r_reflection_probe.h>
#include <database64/db_package.h>
#include "native_world_files.h"
#include "native_world_assemble.h"
#include "native_sun.h"
#include "native_asset_files.h"
#include "native_material_files.h"
#include "native_material.h"
#include "native_technique_files.h"
#include "native_material_sort.h"
#include "native_world_sort.h"
#include "native_surface_transforms.h"
#include "native_world_visibility.h"
#include "native_light_files.h"
#include "native_light_grid.h"
#include "native_grid_collision.h"
#include "native_world_shadows.h"
#include "native_model_files.h"
#include "native_world_models.h"
#include "native_model_cells.h"
#include "native_model_trees.h"
#include "native_dynamic_entity.h"
#include "native_model_pieces.h"
#include "native_physics.h"
#include "native_effect_files.h"
#include <DynEntity/DynEntity_client.h>
#include <xanim/xmodel.h>
#include <stdio.h>
#include <stdlib.h>

struct PieceCompileContext
{
    const char *root;
    LinkerWorldSource *world;
    char *error;
    size_t errorSize;
};

static XModel *CompilePieceModel(const char *name, void *context)
{
    PieceCompileContext *input = (PieceCompileContext *)context;
    LinkerWorldSource *world = input->world;
    for (unsigned int i = 0; i < world->modelCount; ++i)
    {
        if (!_stricmp(world->models[i]->model->name, name))
        {
            return world->models[i]->model;
        }
    }
    LinkerCompiledModel **next = (LinkerCompiledModel **)realloc(world->models, (world->modelCount + 1) * sizeof(LinkerCompiledModel *));
    if (!next)
    {
        return NULL;
    }
    world->models = next;
    LinkerCompiledModel *compiled = NULL;
    if (!Linker_CompileModelFiles(input->root, name, Linker_InternModelString, &world->modelStrings, &compiled, input->error, input->errorSize))
    {
        return NULL;
    }
    world->models[world->modelCount++] = compiled;
    return compiled->model;
}

XModel *Linker_CompileWorldModel(const char *root, LinkerWorldSource *world, const char *name,
                                char *error, size_t errorSize)
{
    PieceCompileContext context = {root, world, error, errorSize};
    return CompilePieceModel(name, &context);
}

struct WorldEffectCompileContext
{
    PieceCompileContext source;
    LinkerEffectFiles *files;
    LinkerCompiledMaterial **materials;
    unsigned int materialCount;
    PhysPreset **presets;
    unsigned int presetCount;
};

static Material *CompileEffectMaterial(const char *name, void *context)
{
    WorldEffectCompileContext *owner = (WorldEffectCompileContext *)context;
    for (unsigned int i = 0; i < owner->materialCount; ++i)
    {
        if (!_stricmp(owner->materials[i]->material->info.name, name)) { return owner->materials[i]->material; }
    }
    LinkerCompiledMaterial **next = (LinkerCompiledMaterial **)realloc(owner->materials, (owner->materialCount + 1) * sizeof(LinkerCompiledMaterial *));
    if (!next) { return NULL; }
    owner->materials = next;
    LinkerCompiledMaterial *material = NULL;
    if (!Linker_CompileMaterialFiles(owner->source.root, name, IMAGE_TRACK_FX, &material, owner->source.error, owner->source.errorSize)) { return NULL; }
    owner->materials[owner->materialCount++] = material;
    return material->material;
}
static XModel *CompileEffectModel(const char *name, void *context)
{
    return CompilePieceModel(name, &((WorldEffectCompileContext *)context)->source);
}
static PhysPreset *CompileEffectPreset(const char *name, void *context)
{
    WorldEffectCompileContext *owner = (WorldEffectCompileContext *)context;
    for (unsigned int i = 0; i < owner->presetCount; ++i)
    {
        if (!_stricmp(owner->presets[i]->name, name)) { return owner->presets[i]; }
    }
    PhysPreset **next = (PhysPreset **)realloc(owner->presets, (owner->presetCount + 1) * sizeof(PhysPreset *));
    if (!next) { return NULL; }
    owner->presets = next;
    char path[1024];
    if (snprintf(path, sizeof(path), "physic/%s", name) >= sizeof(path)) { return NULL; }
    void *bytes = NULL;
    size_t size = 0;
    PhysPreset *preset = NULL;
    const bool valid = Linker_ReadRawAssetFile(owner->source.root, path, &bytes, &size, 1024 * 1024) &&
                       Linker_ImportPhysicsPreset(bytes, size, name, &preset, owner->source.error, owner->source.errorSize);
    free(bytes);
    if (!valid) { return NULL; }
    owner->presets[owner->presetCount++] = preset;
    return preset;
}
static void FreeEffectContext(WorldEffectCompileContext *owner)
{
    if (owner)
    {
        Linker_FreeEffectFiles(owner->files);
        for (unsigned int i = 0; i < owner->materialCount; ++i) { Linker_FreeCompiledMaterial(owner->materials[i]); }
        for (unsigned int i = 0; i < owner->presetCount; ++i) { free(owner->presets[i]); }
        free(owner->materials);
        free(owner->presets);
        free((void *)owner->source.root);
        free(owner);
    }
}

const FxEffectDef *Linker_CompileWorldEffect(const char *root, LinkerWorldSource *world, const char *name,
                                            char *error, size_t errorSize)
{
    if (!world || !root) { return NULL; }
    if (!world->effectContext)
    {
        world->effectContext = (WorldEffectCompileContext *)calloc(1, sizeof(WorldEffectCompileContext));
        if (!world->effectContext) { return NULL; }
        WorldEffectCompileContext *owner = world->effectContext;
        owner->source = {_strdup(root), world, error, errorSize};
        const LinkerFxServices services = {owner, NULL, CompileEffectPreset, CompileEffectMaterial, CompileEffectModel, NULL};
        owner->files = owner->source.root ? Linker_CreateEffectFiles(root, services) : NULL;
    }
    if (!world->effectContext->files) { return NULL; }
    world->effectContext->source.error = error;
    world->effectContext->source.errorSize = errorSize;
    return Linker_CompileEffectFile(world->effectContext->files, name, error, errorSize);
}

static bool CompileDynamicEntities(const char *root, LinkerWorldSource *world, char *error, size_t errorSize)
{
    if (!world->dynamicEntityCount)
    {
        return true;
    }
    world->dynamicPresets = (PhysPreset **)calloc(world->dynamicEntityCount, sizeof(PhysPreset *));
    world->dynamicPieces = (XModelPieces **)calloc(world->dynamicEntityCount, sizeof(XModelPieces *));
    DynEntityDef *definitions = (DynEntityDef *)calloc(world->dynamicEntityCount, sizeof(DynEntityDef));
    bool valid = world->dynamicPresets && world->dynamicPieces && definitions;
    for (unsigned int i = 0; valid && i < world->dynamicEntityCount; ++i)
    {
        const DynEntityCreateParams *params = &world->dynamicEntities[i];
        const FxEffectDef *effect = NULL;
        if (params->destroyFxFile[0])
        {
            effect = Linker_CompileWorldEffect(root, world, params->destroyFxFile, error, errorSize);
            if (!effect) { valid = false; break; }
        }
        XModel *model = NULL;
        if (params->modelName[0] != '*')
        {
            for (unsigned int m = 0; m < world->modelCount; ++m)
            {
                if (!_stricmp(world->models[m]->model->name, params->modelName))
                {
                    model = world->models[m]->model;
                    break;
                }
            }
            if (!model)
            {
                LinkerCompiledModel **next = (LinkerCompiledModel **)realloc(world->models, (world->modelCount + 1) * sizeof(LinkerCompiledModel *));
                valid = next != NULL;
                if (!valid)
                {
                    break;
                }
                world->models = next;
                LinkerCompiledModel *compiled = NULL;
                valid = Linker_CompileModelFiles(root, params->modelName, Linker_InternModelString, &world->modelStrings,
                                                 &compiled, error, errorSize);
                if (!valid)
                {
                    break;
                }
                world->models[world->modelCount++] = compiled;
                model = compiled->model;
            }
        }
        PhysPreset *preset = model ? model->physPreset : NULL;
        if (params->physPresetFile[0] || !preset)
        {
            const char *name = params->physPresetFile[0] ? params->physPresetFile : "default";
            char path[128];
            snprintf(path, sizeof(path), "physic/%s", name);
            void *bytes = NULL;
            size_t size = 0;
            valid = Linker_ReadRawAssetFile(root, path, &bytes, &size, 1024 * 1024) &&
                    Linker_ImportPhysicsPreset(bytes, size, name, &world->dynamicPresets[i], error, errorSize);
            free(bytes);
            preset = world->dynamicPresets[i];
        }
        if (valid && params->destroyPiecesFile[0])
        {
            char path[128];
            snprintf(path, sizeof(path), "xmodelpieces/%s", params->destroyPiecesFile);
            void *bytes = NULL;
            size_t size = 0;
            PieceCompileContext context = {root, world, error, errorSize};
            valid = Linker_ReadRawAssetFile(root, path, &bytes, &size, 16 * 1024 * 1024) &&
                    Linker_ImportModelPieces(bytes, size, params->destroyPiecesFile, CompilePieceModel, &context,
                                            &world->dynamicPieces[i], error, errorSize);
            free(bytes);
        }
        valid = valid && Linker_BuildDynamicEntity(params, world->collision, model, effect, world->dynamicPieces[i], preset, &definitions[i], error, errorSize);
    }
    valid = valid && Linker_AttachDynamicEntities(world->collision, definitions, world->dynamicEntityCount, error, errorSize);
    free(definitions);
    return valid;
}

static bool AssignModelGridLights(LinkerWorldSource *world, char *error, size_t errorSize)
{
    if (!world->sunPrimaryLightIndex)
    {
        return true;
    }
    LinkerGridCollision *collision = NULL;
    bool valid = true;
    for (unsigned int i = 0; valid && i < world->staticModelPlacementCount; ++i)
    {
        GfxStaticModelDrawInst *draw = &world->modelDrawInstances[i];
        const GfxStaticModelInst *instance = &world->modelInstances[i];
        if (draw->primaryLightIndex || ((draw->model->flags & 1) && instance->groundLighting.packed))
        {
            continue;
        }
        if (!collision)
        {
            valid = Linker_CreateGridCollision(world->collision, &collision, error, errorSize);
        }
        float point[3];
        for (unsigned int j = 0; j < 3; ++j)
        {
            point[j] = (float)(((double)instance->mins[j] + instance->maxs[j]) * 0.5);
        }
        uint8_t light = 0;
        valid = valid && Linker_SamplePrimaryLightGrid(world->lightGrid, point, world->metadata->common.primaryLightCount,
                                                      Linker_GridCollisionTrace, collision, &light, error, errorSize);
        if (valid)
        {
            draw->primaryLightIndex = light == 1 ? 1 : 0;
        }
    }
    Linker_FreeGridCollision(collision);
    return valid;
}

static bool CompileWorldMaterial(const char *root, LinkerWorldSource *world, unsigned int index, Material **bindings,
                                 uint8_t *states, unsigned int depth, char *error, size_t errorSize)
{
    if (index >= world->materialCount || states[index] == 1 || depth > 64)
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid or cyclic layered BSP material dependency at index %u", index);
        }
        return false;
    }
    if (bindings[index])
    {
        return true;
    }
    states[index] = 1;
    const char *name = world->geometry->materials[index].material;
    if (!strcmp(name, "$default"))
    {
        name = "$default3d";
    }
    char materialName[DB64_PACKAGE_PATH];
    const int length = snprintf(materialName, sizeof(materialName), "%s%s", name[0] == '*' ? "" : "wc/", name);
    bool valid = length > 0 && (size_t)length < sizeof(materialName);
    for (unsigned int j = 0; valid && j < world->materialCount; ++j)
    {
        if (world->materials[j] && world->materials[j]->material &&
            !_stricmp(world->materials[j]->material->info.name, materialName))
        {
            bindings[index] = world->materials[j]->material;
            states[index] = 2;
            return true;
        }
    }
    if (valid && name[0] != '*')
    {
        valid = Linker_CompileMaterialFiles(root, materialName, IMAGE_TRACK_WORLD, &world->materials[index], error,
                                            errorSize);
    }
    else if (valid)
    {
        LinkerMaterialLayer descriptions[5];
        const Material *layers[5] = {};
        unsigned int layerCount = 0;
        valid = Linker_ParseMaterialLayers(name, world->materialCount, descriptions, &layerCount, error, errorSize);
        uint32_t normalHash = 0;
        for (const char *p = "normalMap"; *p; ++p)
        {
            normalHash = (uint32_t)(*p | 32) ^ (33 * normalHash);
        }
        for (unsigned int i = 0; valid && i < layerCount; ++i)
        {
            valid = CompileWorldMaterial(root, world, descriptions[i].materialIndex, bindings, states, depth + 1, error,
                                         errorSize);
            if (valid)
            {
                layers[i] = bindings[descriptions[i].materialIndex];
                bool hasNormal = false;
                for (unsigned int j = 0; j < layers[i]->textureCount; ++j)
                {
                    const MaterialTextureDef *texture = &layers[i]->textureTable[j];
                    if (texture->nameHash == normalHash && texture->semantic == TS_NORMAL_MAP && texture->u.image)
                    {
                        hasNormal = strcmp(texture->u.image->name, "$identitynormalmap") != 0;
                    }
                }
                valid = hasNormal == descriptions[i].normalMap;
                if (!valid && errorSize)
                {
                    snprintf(error, errorSize, "Layered material '%s' has an incorrect normal-map flag for '%s'", name,
                             layers[i]->info.name);
                }
            }
        }
        char techniqueName[64];
        uint8_t format = 0;
        valid = valid && Linker_BuildLayeredTechniqueName(layers, layerCount, techniqueName, sizeof(techniqueName),
                                                          &format, error, errorSize);
        if (valid)
        {
            world->materials[index] = (LinkerCompiledMaterial *)calloc(1, sizeof(LinkerCompiledMaterial));
            valid = world->materials[index] && layers[0]->stateBitsCount && layers[0]->stateBitsTable;
        }
        if (valid)
        {
            LinkerCompiledMaterial *compiled = world->materials[index];
            valid = Linker_CompileTechniqueSetFiles(root, techniqueName, layers[0]->stateBitsTable[0].loadBits, 0,
                                                    &compiled->techniques, error, errorSize);
            if (valid)
            {
                compiled->techniques->techniqueSet->worldVertFormat = format;
                valid = Linker_BuildLayeredMaterial(materialName, layers, layerCount, compiled->techniques,
                                                    &compiled->material, error, errorSize);
            }
        }
    }
    if (valid)
    {
        bindings[index] = world->materials[index]->material;
        states[index] = 2;
    }
    else if (errorSize && !error[0])
    {
        snprintf(error, errorSize, "Cannot compile BSP material '%s'", materialName);
    }
    return valid;
}

void Linker_FreeWorldSource(LinkerWorldSource *world)
{
    if (world)
    {
        Linker_FreeAssembledWorld(world->assembled);
        FreeEffectContext(world->effectContext);
        Linker_FreeCompiledSun(world->sunEffects);
        for (unsigned int i = 0; world->materials && i < world->materialCount; ++i)
        {
            Linker_FreeCompiledMaterial(world->materials[i]);
        }
        free(world->materials);
        Linker_FreeWorldSurfaceOrder(world->surfaceOrder);
        Linker_FreeWorldVisibility(world->visibility);
        free(world->sunParse);
        free(world->sunLight);
        free(world->staticModelPlacements);
        free(world->dynamicEntities);
        for (unsigned int i = 0; world->dynamicPresets && i < world->dynamicEntityCount; ++i)
        {
            free(world->dynamicPresets[i]);
        }
        free(world->dynamicPresets);
        for (unsigned int i = 0; world->dynamicPieces && i < world->dynamicEntityCount; ++i)
        {
            Linker_FreeModelPieces(world->dynamicPieces[i]);
        }
        free(world->dynamicPieces);
        for (unsigned int i = 0; i < world->modelCount; ++i)
        {
            Linker_FreeCompiledModel(world->models[i]);
        }
        free(world->models);
        free(world->staticModelAssets);
        free(world->modelDrawInstances);
        free(world->modelInstances);
        Linker_FreeModelCells(world->modelCells);
        Linker_FreeModelTrees(world->modelTrees);
        for (int i = 1; i < world->modelStrings.count; ++i)
        {
            free((void *)world->modelStrings.strings[i]);
        }
        free(world->modelStrings.strings);
        for (unsigned int i = 0; i < world->lightDefCount; ++i)
        {
            Linker_FreeLightDefinition(world->lightDefs[i]);
        }
        free(world->lightDefs);
        free(world->nodes);
        Linker_FreeSurfaceShadows(world->surfaceShadows,
                                  world->metadata ? world->metadata->common.primaryLightCount : 0);
        Linker_FreeMapWorlds(world->metadata);
        Linker_FreeRenderCells(world->cells);
        Linker_FreeRenderTrees(world->trees);
        Linker_FreeRenderGroups(world->groups);
        Linker_FreeRenderGeometry(world->geometry);
        Linker_FreeLightmaps(world->lightmaps);
        Linker_FreeReflectionProbes(world->probes);
        Linker_FreeLightRegions(world->regions);
        Linker_FreeLightGrid(world->lightGrid);
        Linker_FreeCollisionGeometry(world->collision);
        free(world);
    }
}

bool Linker_CompileWorldSource(const char *root, const char *bspName, bool layered, LinkerWorldSource **world,
                               char *error, size_t errorSize)
{
    if (!world || (!error && errorSize))
    {
        return false;
    }
    *world = NULL;
    if (errorSize)
    {
        error[0] = 0;
    }
    char normalized[DB64_PACKAGE_PATH];
    void *bsp = NULL;
    size_t size = 0;
    bool valid = root && bspName && DB64_NormalizePath(bspName, normalized, sizeof(normalized)) &&
                 !strncmp(normalized, "maps/", 5);
    const size_t nameLength = valid ? strlen(normalized) : 0;
    valid = valid && nameLength > 7 && !_stricmp(normalized + nameLength - 7, ".d3dbsp");
    valid = valid && Linker_ReadRawAssetFile(root, normalized, &bsp, &size, 512 * 1024 * 1024);
    LinkerWorldSource *result = valid ? (LinkerWorldSource *)calloc(1, sizeof(LinkerWorldSource)) : NULL;
    valid = valid && result;
    if (valid)
    {
        result->sunParse = (SunLightParseParams *)calloc(1, sizeof(SunLightParseParams));
        result->sunLight = (GfxLight *)calloc(1, sizeof(GfxLight));
        valid = result->sunParse && result->sunLight &&
                Linker_ImportSunSettings(bsp, size, result->sunParse, result->sunLight, error, errorSize);
    }
    if (valid)
    {
        valid = Linker_ImportMapWorlds(bsp, size, normalized, &result->metadata, error, errorSize) &&
                Linker_ImportCollisionGeometry(bsp, size, &result->collision, error, errorSize) &&
                Linker_ImportRenderGeometry(bsp, size, layered, &result->geometry, error, errorSize) &&
                Linker_ImportRenderGroups(bsp, size, layered, result->geometry->surfaceCount, &result->groups, error,
                                          errorSize) &&
                Linker_ImportRenderTrees(bsp, size, layered, result->geometry, &result->trees, error, errorSize) &&
                Linker_ImportRenderCells(bsp, size, layered, result->trees, result->groups, &result->cells, error,
                                         errorSize) &&
                Linker_ImportRenderNodes(bsp, size, result->cells->cellCount, result->cells->planeCount, &result->nodes,
                                         &result->nodeWordCount, error, errorSize);
    }
    if (valid)
    {
        const ComWorld *common = &result->metadata->common;
        result->sunPrimaryLightIndex = common->primaryLightCount > 1 && common->primaryLights[1].type == 1;
        valid = Linker_ImportStaticModelPlacements(bsp, size, result->sunPrimaryLightIndex, common->primaryLightCount,
                                                   &result->staticModelPlacements, &result->staticModelPlacementCount,
                                                   error, errorSize) &&
                Linker_ImportDynamicEntityParams(bsp, size, &result->dynamicEntities, &result->dynamicEntityCount,
                                                 error, errorSize) &&
                Linker_ImportLightGrid(bsp, size, result->sunPrimaryLightIndex, common->primaryLightCount,
                                       &result->lightGrid, error, errorSize) &&
                Linker_ImportLightmaps(bsp, size, normalized, result->geometry, &result->lightmaps, error, errorSize) &&
                Linker_ImportLightRegions(bsp, size, common->primaryLightCount, &result->regions, error, errorSize);
        if (valid)
        {
            result->lightGrid->hasLightRegions = result->regions->present;
        }
    }
    ColorCorrectionData *corrections = NULL;
    unsigned int correctionCount = 0;
    if (valid)
    {
        void *csv = NULL;
        size_t csvSize = 0;
        bool found = false;
        if (Linker_ReadRawAssetFile(root, "reflections/reflections.csv", &csv, &csvSize, 1048576, &found))
        {
            valid = Linker_ParseReflectionCorrections(csv, csvSize, &corrections, &correctionCount, error, errorSize);
            free(csv);
        }
        else
        {
            valid = !found;
        }
        valid = valid && Linker_ImportReflectionProbes(bsp, size, normalized, corrections, correctionCount,
                                                       &result->probes, error, errorSize);
    }
    free(corrections);
    free(bsp);
    if (valid)
    {
        const ComWorld *common = &result->metadata->common;
        result->lightDefs = (GfxLightDef **)calloc(common->primaryLightCount, sizeof(GfxLightDef *));
        valid = result->lightDefs != NULL;
        unsigned int lookupStart = 1;
        for (unsigned int i = 0; valid && i < common->primaryLightCount; ++i)
        {
            const char *name = common->primaryLights[i].defName;
            if (!name || !name[0])
            {
                continue;
            }
            bool duplicate = false;
            for (unsigned int j = 0; j < result->lightDefCount; ++j)
            {
                duplicate = duplicate || !_stricmp(name, result->lightDefs[j]->name);
            }
            if (duplicate)
            {
                continue;
            }
            GfxLightDef *definition = NULL;
            valid = Linker_CompileLightDefinition(root, name, lookupStart, &definition, error, errorSize);
            if (valid)
            {
                result->lightDefs[result->lightDefCount++] = definition;
                lookupStart += definition->attenuation.image->width + 2;
                for (unsigned int j = 0; valid && j < result->lightmaps->count; ++j)
                {
                    valid = Linker_StampLightAttenuation(definition, result->lightmaps->lightmaps[j].secondary, error,
                                                         errorSize);
                }
            }
        }
    }
    Material **bindings = NULL;
    uint8_t *materialStates = NULL;
    if (valid)
    {
        result->materialCount = result->geometry->materialCount;
        result->materials = (LinkerCompiledMaterial **)calloc(result->materialCount, sizeof(LinkerCompiledMaterial *));
        bindings = (Material **)calloc(result->materialCount, sizeof(Material *));
        materialStates = (uint8_t *)calloc(result->materialCount, sizeof(uint8_t));
        valid = result->materials && bindings && materialStates;
    }
    for (unsigned int i = 0; valid && i < result->geometry->surfaceCount; ++i)
    {
        const GfxSurface *surface = &result->geometry->surfaces[i];
        valid = surface->reflectionProbeIndex < result->probes->count &&
                surface->primaryLightIndex < result->metadata->common.primaryLightCount;
        if (!valid)
        {
            if (errorSize)
            {
                snprintf(error, errorSize, "BSP surface %u references probe %u/%u or primary light %u/%u", i,
                         surface->reflectionProbeIndex, result->probes->count, surface->primaryLightIndex,
                         result->metadata->common.primaryLightCount);
            }
            break;
        }
        const unsigned int index = result->geometry->materialIndices[i];
        valid = CompileWorldMaterial(root, result, index, bindings, materialStates, 0, error, errorSize);
    }
    if (valid)
    {
        valid = Linker_BindRenderMaterials(result->geometry, bindings, result->materialCount, error, errorSize);
        valid = valid && Linker_TransformMagicPortals(result->geometry, error, errorSize);
        for (unsigned int i = 0; valid && i < result->geometry->surfaceCount; ++i)
        {
            GfxSurface *surface = &result->geometry->surfaces[i];
            if (surface->material->info.gameFlags & 8)
            {
                valid = !surface->primaryLightIndex || surface->primaryLightIndex == result->sunPrimaryLightIndex;
                if (valid)
                {
                    surface->primaryLightIndex = (uint8_t)result->sunPrimaryLightIndex;
                }
            }
        }
    }
    for (unsigned int i = 0; valid && i < result->cells->cellCount; ++i)
    {
        const GfxCell *cell = &result->cells->cells[i];
        for (unsigned int j = 0; valid && j < cell->reflectionProbeCount; ++j)
        {
            valid = cell->reflectionProbes[j] < result->probes->count;
            if (!valid && errorSize)
            {
                snprintf(error, errorSize, "BSP cell %u references reflection probe %u, but only %u probes exist", i,
                         cell->reflectionProbes[j], result->probes->count);
            }
        }
    }
    uint16_t *materialRanks = NULL;
    if (valid)
    {
        valid = Linker_BuildMaterialRanks(bindings, result->materialCount, &materialRanks, error, errorSize) &&
                Linker_SortWorldSurfaces(result->geometry, result->groups, materialRanks, result->materialCount,
                                         &result->surfaceOrder, error, errorSize) &&
                Linker_BuildWorldVisibility(result->geometry, result->groups, result->trees, result->cells,
                                            materialRanks, result->materialCount, result->surfaceOrder,
                                            &result->visibility, error, errorSize);
        valid = valid &&
                Linker_BuildSurfaceShadows(&result->metadata->common, result->sunPrimaryLightIndex, result->regions,
                                           result->surfaceOrder, &result->surfaceShadows, error, errorSize);
    }
    free(materialRanks);
    if (valid)
    {
        result->modelStrings.strings = (const char **)calloc(65536, sizeof(const char *));
        result->modelStrings.count = 1;
        valid = result->modelStrings.strings != NULL;
        if (valid && result->staticModelPlacementCount)
        {
            result->models =
                (LinkerCompiledModel **)calloc(result->staticModelPlacementCount, sizeof(LinkerCompiledModel *));
            result->staticModelAssets = (XModel **)calloc(result->staticModelPlacementCount, sizeof(XModel *));
            valid = result->models && result->staticModelAssets;
        }
        for (unsigned int i = 0; valid && i < result->staticModelPlacementCount; ++i)
        {
            const char *name = result->staticModelPlacements[i].model;
            for (unsigned int j = 0; j < result->modelCount; ++j)
            {
                if (!_stricmp(name, result->models[j]->model->name))
                {
                    result->staticModelAssets[i] = result->models[j]->model;
                    break;
                }
            }
            if (!result->staticModelAssets[i])
            {
                LinkerCompiledModel *compiled = NULL;
                valid = Linker_CompileModelFiles(root, name, Linker_InternModelString, &result->modelStrings, &compiled,
                                                 error, errorSize);
                if (valid)
                {
                    result->models[result->modelCount++] = compiled;
                    result->staticModelAssets[i] = compiled->model;
                }
            }
        }
    }
    free(bindings);
    valid = valid && Linker_BuildStaticModelInstances(result->staticModelPlacements, result->staticModelAssets,
                                                      result->staticModelPlacementCount, &result->modelDrawInstances,
                                                      &result->modelInstances, error, errorSize);
    valid = valid && Linker_BuildModelCells(result->nodes, result->nodeWordCount, result->cells->planes,
                                            result->cells->planeCount, result->cells->cellCount, result->modelInstances,
                                            result->staticModelPlacementCount, &result->modelCells, error, errorSize);
    valid = valid && Linker_BuildModelTrees(result->trees, result->visibility->trees, result->cells, result->modelCells,
                                            result->modelInstances, &result->modelTrees, error, errorSize);
    valid = valid && Linker_AssignModelReflectionProbes(
                         result->nodes, result->nodeWordCount, result->cells, result->probes, result->modelInstances,
                         result->staticModelPlacementCount, result->modelDrawInstances, error, errorSize);
    valid = valid && Linker_AssignModelLocalLights(&result->metadata->common, result->regions, result->modelInstances,
                                                  result->staticModelPlacementCount, result->modelDrawInstances,
                                                  error, errorSize);
    valid = valid && AssignModelGridLights(result, error, errorSize);
    valid = valid && Linker_BuildModelShadows(result->surfaceShadows, result->metadata->common.primaryLightCount,
                                            result->modelDrawInstances, result->staticModelPlacementCount,
                                            error, errorSize);
    valid = valid && Linker_CompileSun(root, normalized, &result->sunEffects, error, errorSize);
    valid = valid && CompileDynamicEntities(root, result, error, errorSize);
    valid = valid && Linker_AssembleWorldGeometry(result, normalized, &result->assembled, error, errorSize);
    valid = valid && Linker_BuildStaticModelCollision(result->modelDrawInstances, result->staticModelPlacementCount,
                                                      &result->collision->staticModelList, error, errorSize);
    if (valid)
    {
        result->collision->numStaticModels = result->staticModelPlacementCount;
    }
    free(materialStates);
    if (!valid)
    {
        Linker_FreeWorldSource(result);
        if (errorSize && !error[0])
        {
            snprintf(error, errorSize,
                     "Cannot compile world source '%s': missing input or invalid dependency reference",
                     bspName ? bspName : "");
        }
        return false;
    }
    *world = result;
    return true;
}

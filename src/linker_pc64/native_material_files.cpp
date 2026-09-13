#include <universal/q_shared.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_image.h>
#include <database64/db_package.h>
#include "native_asset_files.h"
#include "native_material_files.h"
#include "native_material.h"
#include "native_technique_files.h"
#include "native_image.h"
#include "native_water.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void Linker_FreeCompiledMaterial(LinkerCompiledMaterial *compiled)
{
    if (!compiled)
    {
        return;
    }
    Linker_FreeMaterial(compiled->material);
    Linker_FreeCompiledTechniqueSet(compiled->techniques);
    for (unsigned int i = 0; i < compiled->imageCount; ++i)
    {
        Linker_FreeImage(compiled->images[i]);
    }
    for (unsigned int i = 0; i < compiled->waterCount; ++i)
    {
        Linker_FreeWater(compiled->waters[i]);
    }
    free(compiled);
}

static bool CompileMaterialReference(const char *name, LinkerCompiledMaterial **compiled)
{
    if (!name[0] || (name[0] == ',' && !name[1]))
    {
        return false;
    }
    LinkerCompiledMaterial *result = (LinkerCompiledMaterial *)calloc(1, sizeof(LinkerCompiledMaterial));
    if (!result)
    {
        return false;
    }
    const char *plainName = name[0] == ',' ? name + 1 : name;
    result->material = (Material *)calloc(1, sizeof(Material) + strlen(plainName) + 2);
    if (!result->material)
    {
        free(result);
        return false;
    }
    char *referenceName = (char *)(result->material + 1);
    referenceName[0] = ',';
    strcpy(referenceName + 1, plainName);
    result->material->info.name = referenceName;
    *compiled = result;
    return true;
}

bool Linker_CompileMaterialFiles(const char *root, const char *name, unsigned char imageTrack,
                                 LinkerCompiledMaterial **compiled, char *error, size_t errorSize,
                                 const char *language)
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
    if (!root || !name || !DB64_NormalizePath(name, normalized, sizeof(normalized)))
    {
        return false;
    }
    if (normalized[0] == ',')
    {
        return CompileMaterialReference(normalized, compiled);
    }
    const char *prefixes[5] = {"", "m/", "mc/", "w/", "wc/"};
    const char *techPrefixes[5] = {"", "m_", "mc_", "w_", "wc_"};
    unsigned int type = 0;
    for (unsigned int i = 1; i < 5; ++i)
    {
        if (!strncmp(normalized, prefixes[i], strlen(prefixes[i])))
        {
            type = i;
            break;
        }
    }
    snprintf(relative, sizeof(relative), "materials/%s", normalized + strlen(prefixes[type]));
    void *data = NULL;
    size_t size = 0;
    MaterialRaw *raw = NULL;
    LinkerMaterialTexture *textures = NULL;
    unsigned int textureCount = 0;
    bool found = false;
    const bool read = Linker_ReadLocalizedAssetFile(root, relative, language, &data, &size, 16 * 1024 * 1024, &found);
    // The retail startup zone carries this deferred menu material as a comma reference.
    // Prefer an authored material when available; other missing materials remain errors.
    if (!read && !found && !strcmp(normalized, "$levelbriefing"))
    {
        return CompileMaterialReference(normalized, compiled);
    }
    bool valid = read &&
                 Linker_ReadMaterialSource(data, size, &raw, error, errorSize) &&
                 Linker_ImportMaterialTextures(data, size, &textures, &textureCount, error, errorSize);
    LinkerCompiledMaterial *result = valid ? (LinkerCompiledMaterial *)calloc(1, sizeof(LinkerCompiledMaterial)) : NULL;
    valid = valid && result;
    if (valid)
    {
        snprintf(relative, sizeof(relative), "%s%s", techPrefixes[type], (char *)raw + raw->techSetNameOffset);
        valid = Linker_CompileTechniqueSetFiles(root, relative, raw->refStateBits, raw->info.toolFlags,
                                                &result->techniques, error, errorSize);
    }
    MaterialTextureDef dependencies[255] = {};
    for (unsigned int i = 0; valid && i < textureCount; ++i)
    {
        const LinkerMaterialTexture *texture = &textures[i];
        if (texture->semantic == TS_WATER_MAP)
        {
            char waterName[2048];
            snprintf(waterName, sizeof(waterName), "water/%s/%u", normalized, i);
            water_t *water = NULL;
            valid = Linker_CompileWater(texture, waterName, &water, error, errorSize);
            if (valid)
            {
                result->waters[result->waterCount++] = water;
                dependencies[i].u.water = water;
            }
            continue;
        }
        for (unsigned int j = 0; j < i; ++j)
        {
            if (textures[j].semantic == texture->semantic && !strcmp(textures[j].image, texture->image))
            {
                dependencies[i].u = dependencies[j].u;
                break;
            }
        }
        if (dependencies[i].u.image)
        {
            continue;
        }
        snprintf(relative, sizeof(relative), "images/%s.iwi", texture->image);
        void *imageBytes = NULL;
        size_t imageSize = 0;
        GfxImage *image = NULL;
        valid = texture->image[0] == '$' ?
                Linker_CreateBuiltinImage(texture->image, texture->semantic, imageTrack, &image, error, errorSize) :
                Linker_ReadLocalizedAssetFile(root, relative, language, &imageBytes, &imageSize, 256 * 1024 * 1024) &&
                Linker_ImportImage(imageBytes, imageSize, texture->image, texture->semantic, imageTrack, &image, error,
                                   errorSize);
        if (!valid && errorSize && !error[0])
        {
            snprintf(error, errorSize, "Cannot read %s", relative);
        }
        free(imageBytes);
        if (valid)
        {
            result->images[result->imageCount++] = image;
            dependencies[i].u.image = image;
        }
    }
    valid = valid && Linker_BuildMaterial(data, size, type, result->techniques, dependencies, textureCount,
                                          &result->material, error, errorSize);
    free(textures);
    free(raw);
    free(data);
    if (!valid)
    {
        Linker_FreeCompiledMaterial(result);
        if (errorSize)
        {
            char detail[512];
            snprintf(detail, sizeof(detail), "%s", error[0] ? error : "Cannot read material or image dependency");
            snprintf(error, errorSize, "%s: %s", normalized, detail);
        }
        return false;
    }
    *compiled = result;
    return true;
}

#include <universal/q_shared.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_image.h>
#include "native_material.h"
#include "native_technique_files.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <limits.h>
#include <universal/surfaceflags.h>

bool Linker_ParseMaterialLayers(const char *name, unsigned int materialCount, LinkerMaterialLayer layers[5],
                                unsigned int *layerCount, char *error, size_t errorSize)
{
    if (!layers || !layerCount || (!error && errorSize))
    {
        return false;
    }
    *layerCount = 0;
    if (errorSize)
    {
        error[0] = 0;
    }
    bool valid = name && *name == '*';
    const char *cursor = valid ? name + 1 : NULL;
    LinkerMaterialLayer parsed[5] = {};
    unsigned int count = 0;
    while (valid)
    {
        valid = count < 5 && *cursor >= '0' && *cursor <= '9';
        if (!valid)
        {
            break;
        }
        unsigned int index = 0;
        while (*cursor >= '0' && *cursor <= '9')
        {
            const unsigned int digit = *cursor++ - '0';
            if (index > (UINT_MAX - digit) / 10)
            {
                valid = false;
                break;
            }
            index = index * 10 + digit;
        }
        valid = valid && index < materialCount;
        parsed[count].materialIndex = index;
        parsed[count++].normalMap = *cursor == 'n';
        if (*cursor == 'n')
        {
            ++cursor;
        }
        if (!*cursor)
        {
            break;
        }
        valid = valid && *cursor++ == '_';
    }
    if (!valid || !count)
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid layered BSP material name '%s'", name ? name : "");
        }
        return false;
    }
    memcpy(layers, parsed, count * sizeof(LinkerMaterialLayer));
    *layerCount = count;
    return true;
}

bool Linker_BuildLayeredTechniqueName(const Material *const *layers, unsigned int layerCount, char *name,
                                      size_t nameSize, uint8_t *worldFormat, char *error, size_t errorSize)
{
    if (!name || !nameSize || !worldFormat || (!error && errorSize))
    {
        return false;
    }
    name[0] = 0;
    *worldFormat = 0;
    if (errorSize)
    {
        error[0] = 0;
    }
    bool valid = layers && layerCount && layerCount <= 5;
    char result[64] = {};
    size_t length = 0;
    unsigned int normals = 0;
    for (unsigned int i = 0; valid && i < layerCount; ++i)
    {
        valid = layers[i] && layers[i]->techniqueSet && layers[i]->techniqueSet->name;
        if (!valid)
        {
            break;
        }
        const char *tech = layers[i]->techniqueSet->name;
        if (!strncmp(tech, "wc_", 3))
        {
            tech += 3;
        }
        else if (!strncmp(tech, "w_", 2))
        {
            tech += 2;
        }
        else
        {
            valid = false;
            break;
        }
        const char *chunk = NULL;
        if (!strcmp(tech, "unlit_multiply"))
        {
            chunk = "m0c0";
        }
        else if (!strncmp(tech, "l_sm_", 5))
        {
            chunk = tech + 5;
            const char *p = chunk;
            valid = strlen(p) >= 4 && strchr("abrt", p[0]) && p[1] == '0' && p[2] == 'c' && p[3] == '0';
            if (valid)
            {
                p += 4;
                if (!strncmp(p, "d0", 2))
                {
                    p += 2;
                }
                if (!strncmp(p, "n0", 2))
                {
                    ++normals;
                    p += 2;
                }
                if (!strncmp(p, "s0", 2))
                {
                    p += 2;
                }
                valid = !*p;
            }
        }
        else
        {
            valid = false;
        }
        if (!valid)
        {
            break;
        }
        const char *prefix = i ? "_" : (!strcmp(chunk, "m0c0") ? "" : "l_sm_");
        const size_t prefixSize = strlen(prefix), chunkSize = strlen(chunk);
        valid = prefixSize + chunkSize < sizeof(result) - length;
        if (valid)
        {
            memcpy(result + length, prefix, prefixSize);
            length += prefixSize;
            for (size_t j = 0; j < chunkSize; ++j)
            {
                result[length++] = chunk[j] == '0' ? (char)('0' + i) : chunk[j];
            }
        }
    }
    valid = valid && normals <= 3 && length < nameSize;
    if (!valid)
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Unsupported layered world technique set or vertex format");
        }
        return false;
    }
    const uint8_t bases[5] = {0, 1, 3, 6, 9};
    *worldFormat = (uint8_t)(bases[layerCount - 1] + (normals ? normals - 1 : 0));
    memcpy(name, result, length + 1);
    return true;
}

void Linker_FreeMaterial(Material *material)
{
    if (material)
    {
        free(material->textureTable);
        free(material->constantTable);
        free(material->stateBitsTable);
        free(material);
    }
}

static int CompareNativeTextures(const void *left, const void *right)
{
    const uint32_t a = ((const MaterialTextureDef *)left)->nameHash;
    const uint32_t b = ((const MaterialTextureDef *)right)->nameHash;
    return a < b ? -1 : a > b ? 1 : 0;
}

static int CompareNativeConstants(const void *left, const void *right)
{
    const uint32_t a = ((const MaterialConstantDef *)left)->nameHash;
    const uint32_t b = ((const MaterialConstantDef *)right)->nameHash;
    return a < b ? -1 : a > b ? 1 : 0;
}

static bool MaterialHasArguments(const Material *material)
{
    for (unsigned int type = 0; type < TECHNIQUE_COUNT; ++type)
    {
        const MaterialTechnique *technique = material->techniqueSet->techniques[type];
        if (!technique)
        {
            continue;
        }
        for (unsigned int p = 0; p < technique->passCount; ++p)
        {
            const MaterialPass *pass = &technique->passArray[p];
            const unsigned int count = pass->perPrimArgCount + pass->perObjArgCount + pass->stableArgCount;
            if (count && !pass->args)
            {
                return false;
            }
            for (unsigned int a = 0; a < count; ++a)
            {
                const MaterialShaderArgument *argument = &pass->args[a];
                bool found = false;
                if (argument->type == MTL_ARG_MATERIAL_PIXEL_SAMPLER)
                {
                    for (unsigned int t = 0; t < material->textureCount; ++t)
                    {
                        found = found || material->textureTable[t].nameHash == argument->u.nameHash;
                    }
                }
                else if (argument->type == MTL_ARG_MATERIAL_VERTEX_CONST ||
                         argument->type == MTL_ARG_MATERIAL_PIXEL_CONST)
                {
                    for (unsigned int c = 0; c < material->constantCount; ++c)
                    {
                        found = found || material->constantTable[c].nameHash == argument->u.nameHash;
                    }
                }
                else
                {
                    continue;
                }
                if (!found)
                {
                    return false;
                }
            }
        }
    }
    return true;
}

bool Linker_BuildMaterial(const void *data, size_t size, unsigned int materialType,
                          const LinkerCompiledTechniqueSet *techniques, const MaterialTextureDef *dependencies,
                          unsigned int dependencyCount, Material **material, char *error, size_t errorSize)
{
    if (!material || (!error && errorSize))
    {
        return false;
    }
    *material = NULL;
    if (errorSize)
    {
        error[0] = 0;
    }
    if (materialType >= 5 || !techniques || !techniques->techniqueSet || (!dependencies && dependencyCount))
    {
        return false;
    }
    const char *prefixes[5] = {"", "m/", "mc/", "w/", "wc/"};
    const char *techPrefixes[5] = {"", "m_", "mc_", "w_", "wc_"};
    MaterialRaw *raw = NULL;
    LinkerMaterialTexture *textures = NULL;
    unsigned int textureCount = 0, constantCount = 0;
    Material *result = NULL;
    bool valid = Linker_ReadMaterialSource(data, size, &raw, error, errorSize);
    if (valid)
    {
        char techniqueName[512];
        snprintf(techniqueName, sizeof(techniqueName), "%s%s", techPrefixes[materialType],
                 (char *)raw + raw->techSetNameOffset);
        valid = raw->info.sortKey < 64 && SURF_TYPEINDEX(raw->info.surfaceFlags) < 29 &&
                raw->textureCount == dependencyCount && techniques->techniqueSet->name &&
                !strcmp(techniqueName, techniques->techniqueSet->name);
    }
    if (valid)
    {
        const char *name = (char *)raw + raw->info.nameOffset;
        const size_t nameSize = strlen(prefixes[materialType]) + strlen(name) + 1;
        result = (Material *)calloc(1, sizeof(Material) + nameSize);
        valid = result != NULL;
        if (valid)
        {
            result->info.name = (char *)(result + 1);
            snprintf((char *)result->info.name, nameSize, "%s%s", prefixes[materialType], name);
            result->info.gameFlags = raw->info.gameFlags & ~0x40;
            if (materialType != 3 && materialType != 4)
            {
                result->info.gameFlags &= ~2;
            }
            result->info.sortKey = raw->info.sortKey;
            result->info.textureAtlasRowCount = raw->info.textureAtlasRowCount;
            result->info.textureAtlasColumnCount = raw->info.textureAtlasColumnCount;
            const unsigned int surface = SURF_TYPEINDEX(raw->info.surfaceFlags);
            result->info.surfaceTypeBits = surface ? 1 << (surface - 1) : 0;
            valid =
                Linker_ImportMaterialConstants(data, size, &result->constantTable, &constantCount, error, errorSize) &&
                Linker_ImportMaterialTextures(data, size, &textures, &textureCount, error, errorSize);
        }
    }
    if (valid && textureCount)
    {
        result->textureTable = (MaterialTextureDef *)calloc(textureCount, sizeof(MaterialTextureDef));
        valid = result->textureTable != NULL;
    }
    for (unsigned int i = 0; valid && i < textureCount; ++i)
    {
        const LinkerMaterialTexture *input = &textures[i];
        MaterialTextureDef *texture = &result->textureTable[i];
        valid = input->name[0] && (input->samplerState & 7) && dependencies[i].u.image;
        if (!valid)
        {
            break;
        }
        texture->nameHash = input->nameHash;
        texture->nameStart = input->name[0];
        texture->nameEnd = input->name[strlen(input->name) - 1];
        texture->semantic = input->semantic;
        texture->samplerState = input->samplerState;
        texture->u = dependencies[i].u;
    }
    if (valid)
    {
        result->textureCount = (uint8_t)textureCount;
        result->constantCount = (uint8_t)constantCount;
        if (textureCount)
        {
            qsort(result->textureTable, textureCount, sizeof(MaterialTextureDef), CompareNativeTextures);
        }
        if (constantCount)
        {
            qsort(result->constantTable, constantCount, sizeof(MaterialConstantDef), CompareNativeConstants);
        }
        for (unsigned int i = 1; i < textureCount; ++i)
        {
            valid = valid && result->textureTable[i - 1].nameHash != result->textureTable[i].nameHash;
        }
        for (unsigned int i = 1; i < constantCount; ++i)
        {
            valid = valid && result->constantTable[i - 1].nameHash != result->constantTable[i].nameHash;
        }
        valid =
            valid && Linker_BuildMaterialState(techniques, result, error, errorSize) && MaterialHasArguments(result);
        if (valid && !(raw->info.surfaceFlags & SURF_NOCASTSHADOW) &&
            result->techniqueSet->techniques[TECHNIQUE_BUILD_SHADOWMAP_DEPTH] && !(result->stateFlags & 4))
        {
            result->info.gameFlags |= 0x40;
        }
    }
    free(textures);
    free(raw);
    if (!valid)
    {
        Linker_FreeMaterial(result);
        if (errorSize && !error[0])
        {
            snprintf(error, errorSize, "Invalid material metadata, dependencies, or missing shader arguments");
        }
        return false;
    }
    *material = result;
    return true;
}

bool Linker_BuildMaterialState(const LinkerCompiledTechniqueSet *compiled, Material *material, char *error,
                               size_t errorSize)
{
    if (!compiled || !compiled->techniqueSet || !material || material->stateBitsTable || (!error && errorSize))
    {
        return false;
    }
    GfxStateBits table[255] = {};
    uint8_t entries[TECHNIQUE_COUNT];
    memset(entries, 255, sizeof(entries));
    unsigned int count = 0;
    int cullFlags = -1;
    uint8_t stateFlags = 0;
    bool valid = true;
    const MaterialTechniqueSet *set = compiled->techniqueSet;
    for (unsigned int type = 0; valid && type < TECHNIQUE_COUNT; ++type)
    {
        const MaterialTechnique *technique = set->techniques[type];
        if (!technique)
        {
            continue;
        }
        const LinkerCompiledTechnique *input = compiled->techniques[type];
        if (!input || input->technique != technique || !technique->passCount || technique->passCount > 32)
        {
            valid = false;
            break;
        }
        unsigned int scan = 0, matched = 0;
        for (; scan < count; ++scan)
        {
            matched = count - scan;
            if (matched > technique->passCount)
            {
                matched = technique->passCount;
            }
            if (!memcmp(table + scan, input->stateBits, matched * sizeof(GfxStateBits)))
            {
                break;
            }
        }
        if (scan == count)
        {
            matched = 0;
        }
        const unsigned int additional = technique->passCount - matched;
        if (scan >= 255 || additional > 255 - count)
        {
            valid = false;
            break;
        }
        memcpy(table + count, input->stateBits + matched, additional * sizeof(GfxStateBits));
        count += additional;
        entries[type] = (uint8_t)scan;
        const unsigned int cull = table[scan].loadBits[0] & 0xC000;
        const int flags = cull == 0x8000 ? 1 : cull == 0xC000 ? 2 : 0;
        if (type >= TECHNIQUE_LIT_BEGIN && type < TECHNIQUE_LIT_END)
        {
            valid = cullFlags == -1 || cullFlags == flags;
            cullFlags = flags;
        }
        if (type == TECHNIQUE_BUILD_SHADOWMAP_DEPTH)
        {
            stateFlags |= flags << 6;
        }
        for (unsigned int p = 0; p < technique->passCount; ++p)
        {
            const unsigned int bits = table[scan + p].loadBits[1];
            if ((bits & 0x3D) || !(bits & 2))
            {
                stateFlags |= 16;
            }
            if (bits & 0xC0)
            {
                stateFlags |= 32;
            }
        }
    }
    const bool lit = set->techniques[TECHNIQUE_LIT] != NULL;
    for (unsigned int type = TECHNIQUE_LIT_BEGIN; type < TECHNIQUE_LIT_INSTANCED; ++type)
    {
        valid = valid && lit == (set->techniques[type] != NULL);
    }
    if (!valid)
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid material technique state or more than 255 state entries");
        }
        return false;
    }
    if (cullFlags >= 0)
    {
        stateFlags |= cullFlags;
    }
    if (count)
    {
        const unsigned int entry = set->techniques[TECHNIQUE_UNLIT] ? entries[TECHNIQUE_UNLIT] : 0;
        if (table[entry].loadBits[1] & 0x30)
        {
            stateFlags |= 4;
        }
        if (table[entry].loadBits[1] & 1)
        {
            stateFlags |= 8;
        }
    }
    GfxStateBits *owned = count ? (GfxStateBits *)malloc(count * sizeof(GfxStateBits)) : NULL;
    if (count && !owned)
    {
        return false;
    }
    if (count)
    {
        memcpy(owned, table, count * sizeof(GfxStateBits));
    }
    memcpy(material->stateBitsEntry, entries, sizeof(entries));
    material->stateBitsCount = (uint8_t)count;
    material->stateBitsTable = owned;
    material->stateFlags = stateFlags;
    material->techniqueSet = compiled->techniqueSet;
    material->cameraRegion = lit ? (material->info.sortKey >= 24 ? 1 : 0) : set->techniques[TECHNIQUE_EMISSIVE] ? 2 : 3;
    return true;
}

static bool MaterialRange(size_t size, uint32_t offset, size_t bytes)
{
    return offset >= sizeof(MaterialRaw) && offset <= size && bytes <= size - offset;
}
static bool MaterialString(const uint8_t *data, size_t size, uint32_t offset)
{
    return MaterialRange(size, offset, 1) && data[offset] && memchr(data + offset, 0, size - offset);
}

bool Linker_ReadMaterialSource(const void *data, size_t size, MaterialRaw **material, char *error, size_t errorSize)
{
    static_assert(sizeof(MaterialRaw) == 64);
    static_assert(sizeof(MaterialTextureDefRaw) == 12);
    static_assert(sizeof(MaterialConstantDefRaw) == 20);
    if (!material || (!error && errorSize))
    {
        return false;
    }
    *material = NULL;
    MaterialRaw header = {};
    const uint8_t *bytes = (const uint8_t *)data;
    bool valid = data && size >= sizeof(MaterialRaw) && size <= INT_MAX;
    if (valid)
    {
        memcpy(&header, data, sizeof(MaterialRaw));
        valid = header.textureCount <= 255 && header.constantCount <= 255 &&
                MaterialString(bytes, size, header.info.nameOffset) &&
                MaterialString(bytes, size, header.techSetNameOffset) &&
                (!header.info.refImageNameOffset || MaterialString(bytes, size, header.info.refImageNameOffset)) &&
                (!header.textureCount ||
                 MaterialRange(size, header.textureTableOffset, header.textureCount * sizeof(MaterialTextureDefRaw))) &&
                (!header.constantCount || MaterialRange(size, header.constantTableOffset,
                                                        header.constantCount * sizeof(MaterialConstantDefRaw))) &&
                isfinite(header.info.maxDeformMove) && isfinite(header.info.tessSize);
    }
    for (unsigned int i = 0; valid && i < header.textureCount; ++i)
    {
        MaterialTextureDefRaw texture;
        memcpy(&texture, bytes + header.textureTableOffset + i * sizeof(MaterialTextureDefRaw),
               sizeof(MaterialTextureDefRaw));
        valid = MaterialString(bytes, size, texture.nameOffset) && (texture.samplerState & 7) != 0;
        if (texture.semantic == TS_WATER_MAP)
        {
            // Raw water records have a 32-bit runtime pointer slot, even for the x64 compiler.
            valid = valid && MaterialRange(size, texture.u.waterDefOffset, 32);
            if (valid)
            {
                int32_t width;
                float parameters[6];
                memcpy(&width, bytes + texture.u.waterDefOffset, sizeof(int32_t));
                memcpy(parameters, bytes + texture.u.waterDefOffset + 4, sizeof(float[6]));
                valid = width > 0 && width <= 4096 && (width & (width - 1)) == 0;
                for (int j = 0; j < 6; ++j)
                {
                    valid = valid && isfinite(parameters[j]);
                }
            }
        }
        else
        {
            valid = valid && MaterialString(bytes, size, texture.u.imageNameOffset);
        }
    }
    for (unsigned int i = 0; valid && i < header.constantCount; ++i)
    {
        MaterialConstantDefRaw constant;
        memcpy(&constant, bytes + header.constantTableOffset + i * sizeof(MaterialConstantDefRaw),
               sizeof(MaterialConstantDefRaw));
        valid = MaterialString(bytes, size, constant.nameOffset);
        for (int j = 0; j < 4; ++j)
        {
            valid = valid && isfinite(constant.literal[j]);
        }
    }
    if (valid)
    {
        *material = (MaterialRaw *)malloc(size);
        valid = *material != NULL;
        if (valid)
        {
            memcpy(*material, data, size);
        }
    }
    if (!valid && errorSize)
    {
        snprintf(error, errorSize, "Invalid, truncated or unallocatable raw material");
    }
    return valid;
}

bool Linker_ImportMaterialConstants(const void *data, size_t size, MaterialConstantDef **constants, unsigned int *count,
                                    char *error, size_t errorSize)
{
    if (!constants || !count || (!error && errorSize))
    {
        return false;
    }
    *constants = NULL;
    *count = 0;
    MaterialRaw *source = NULL;
    if (!Linker_ReadMaterialSource(data, size, &source, error, errorSize))
    {
        return false;
    }
    MaterialConstantDef *result =
        source->constantCount ? (MaterialConstantDef *)calloc(source->constantCount, sizeof(MaterialConstantDef))
                              : NULL;
    if (source->constantCount && !result)
    {
        free(source);
        if (errorSize)
        {
            snprintf(error, errorSize, "Cannot allocate native material constants");
        }
        return false;
    }
    for (unsigned int i = 0; i < source->constantCount; ++i)
    {
        MaterialConstantDefRaw raw;
        memcpy(&raw, (const uint8_t *)source + source->constantTableOffset + i * sizeof(MaterialConstantDefRaw),
               sizeof(MaterialConstantDefRaw));
        const char *name = (const char *)source + raw.nameOffset;
        uint32_t hash = 0;
        for (const char *p = name; *p; ++p)
        {
            hash = (uint32_t)((int)*p | 0x20) ^ (33 * hash);
        }
        result[i].nameHash = hash;
        const size_t length = strlen(name);
        memcpy(result[i].name, name, length < sizeof(result[i].name) ? length : sizeof(result[i].name));
        memcpy(result[i].literal, raw.literal, sizeof(float[4]));
    }
    *constants = result;
    *count = source->constantCount;
    free(source);
    return true;
}

bool Linker_BuildLayeredMaterial(const char *name, const Material *const *layers, unsigned int layerCount,
                                 const LinkerCompiledTechniqueSet *techniques, Material **material, char *error,
                                 size_t errorSize)
{
    if (!material || (!error && errorSize))
    {
        return false;
    }
    *material = NULL;
    if (errorSize)
    {
        error[0] = 0;
    }
    bool valid = name && name[0] && strlen(name) < 1024 && layers && layerCount && layerCount <= 5 && techniques &&
                 techniques->techniqueSet;
    uint32_t tintHash = 0;
    for (const char *p = "colorTint"; *p; ++p)
    {
        tintHash = (uint32_t)(*p | 32) ^ (33 * tintHash);
    }
    unsigned int textureCount = 0, constantCount = 0;
    bool hasTint[5] = {};
    for (unsigned int i = 0; valid && i < layerCount; ++i)
    {
        const Material *layer = layers[i];
        valid = layer && layer->techniqueSet && (!layer->textureCount || layer->textureTable) &&
                (!layer->constantCount || layer->constantTable);
        if (!valid)
        {
            break;
        }
        textureCount += layer->textureCount;
        constantCount += layer->constantCount;
        for (unsigned int j = 0; j < layer->constantCount; ++j)
        {
            hasTint[i] = hasTint[i] || layer->constantTable[j].nameHash == tintHash;
        }
        constantCount += !hasTint[i];
    }
    valid = valid && textureCount <= 255 && constantCount <= 255;
    Material *result = valid ? (Material *)calloc(1, sizeof(Material) + strlen(name) + 1) : NULL;
    valid = valid && result;
    if (valid)
    {
        result->info = layers[0]->info;
        result->info.name = (char *)(result + 1);
        strcpy((char *)result->info.name, name);
        result->techniqueSet = techniques->techniqueSet;
        result->textureCount = (uint8_t)textureCount;
        result->constantCount = (uint8_t)constantCount;
        result->textureTable =
            textureCount ? (MaterialTextureDef *)calloc(textureCount, sizeof(MaterialTextureDef)) : NULL;
        result->constantTable = (MaterialConstantDef *)calloc(constantCount, sizeof(MaterialConstantDef));
        valid = (!textureCount || result->textureTable) && result->constantTable;
    }
    unsigned int tex = 0, constant = 0;
    uint8_t flagsOr = 0, flagsAnd = 255;
    for (unsigned int i = 0; valid && i < layerCount; ++i)
    {
        const Material *layer = layers[i];
        flagsOr |= layer->info.gameFlags;
        flagsAnd &= layer->info.gameFlags;
        result->info.surfaceTypeBits |= layer->info.surfaceTypeBits;
        for (unsigned int j = 0; j < layer->textureCount; ++j)
        {
            MaterialTextureDef *dest = &result->textureTable[tex++];
            *dest = layer->textureTable[j];
            if ((dest->samplerState & 0x18) == 8 && (dest->semantic == TS_COLOR_MAP || dest->semantic == TS_NORMAL_MAP))
            {
                dest->samplerState = (dest->samplerState & ~0x18) | 0x10;
            }
            if (i)
            {
                dest->nameEnd = (char)('0' + i);
                dest->nameHash = (uint32_t)dest->nameEnd ^ (33 * dest->nameHash);
            }
        }
        for (unsigned int j = 0; j < (unsigned int)layer->constantCount + !hasTint[i]; ++j)
        {
            MaterialConstantDef *dest = &result->constantTable[constant++];
            if (j < layer->constantCount)
            {
                *dest = layer->constantTable[j];
            }
            else
            {
                memcpy(dest->name, "colorTint", sizeof("colorTint"));
                dest->nameHash = tintHash;
                for (int a = 0; a < 4; ++a)
                {
                    dest->literal[a] = 1;
                }
            }
            if (i)
            {
                for (unsigned int k = 0; k < sizeof(dest->name); ++k)
                {
                    if (!dest->name[k])
                    {
                        dest->name[k] = (char)('0' + i);
                        break;
                    }
                }
                dest->nameHash = (uint32_t)('0' + i) ^ (33 * dest->nameHash);
            }
        }
    }
    LinkerCompiledTechniqueSet derived = {};
    LinkerCompiledTechnique derivedTechniques[TECHNIQUE_COUNT] = {};
    if (valid)
    {
        result->info.gameFlags = (flagsOr & ~4) | (flagsAnd & 4);
        derived.techniqueSet = techniques->techniqueSet;
    }
    for (unsigned int type = 0; valid && type < TECHNIQUE_COUNT; ++type)
    {
        MaterialTechnique *technique = techniques->techniqueSet->techniques[type];
        if (!technique)
        {
            continue;
        }
        const Material *base = layers[0];
        const unsigned int entry = base->stateBitsEntry[type];
        valid = technique->passCount == 1 && base->techniqueSet->techniques[type] &&
                base->techniqueSet->techniques[type]->passCount == 1 && base->stateBitsTable &&
                entry < base->stateBitsCount;
        if (!valid)
        {
            break;
        }
        uint32_t bits[2];
        memcpy(bits, base->stateBitsTable[entry].loadBits, sizeof(bits));
        if ((bits[0] & 0xF0) == 0x10)
        {
            for (unsigned int i = 0; (bits[0] & 0x3800) != 0x800 && valid && i < layerCount; ++i)
            {
                const unsigned int other = layers[i]->stateBitsEntry[type];
                if (other == 255)
                {
                    continue;
                }
                valid = layers[i]->stateBitsTable && other < layers[i]->stateBitsCount;
                if (valid && (layers[i]->stateBitsTable[other].loadBits[0] & 0xF0) != 0x10)
                {
                    bits[0] = (bits[0] & 0xFFFFC000) | 0x902 | (layers[i]->stateBitsTable[other].loadBits[0] & 0xF0);
                    break;
                }
            }
        }
        else if ((bits[0] & 0xF) != 1)
        {
            bits[0] = (bits[0] & ~0xF) | 2;
        }
        derivedTechniques[type].technique = technique;
        memcpy(derivedTechniques[type].stateBits[0], bits, sizeof(bits));
        derived.techniques[type] = &derivedTechniques[type];
    }
    if (valid)
    {
        qsort(result->textureTable, textureCount, sizeof(MaterialTextureDef), CompareNativeTextures);
        qsort(result->constantTable, constantCount, sizeof(MaterialConstantDef), CompareNativeConstants);
        for (unsigned int i = 1; valid && i < textureCount; ++i)
        {
            valid = result->textureTable[i - 1].nameHash != result->textureTable[i].nameHash;
        }
        for (unsigned int i = 1; valid && i < constantCount; ++i)
        {
            valid = result->constantTable[i - 1].nameHash != result->constantTable[i].nameHash;
        }
        valid = valid && Linker_BuildMaterialState(&derived, result, error, errorSize) && MaterialHasArguments(result);
    }
    if (!valid)
    {
        Linker_FreeMaterial(result);
        if (errorSize && !error[0])
        {
            snprintf(error, errorSize, "Invalid layered material tables, passes or shader arguments");
        }
        return false;
    }
    *material = result;
    return true;
}

bool Linker_ImportMaterialTextures(const void *data, size_t size, LinkerMaterialTexture **textures, unsigned int *count,
                                   char *error, size_t errorSize)
{
    if (!textures || !count || (!error && errorSize))
    {
        return false;
    }
    *textures = NULL;
    *count = 0;
    MaterialRaw *source = NULL;
    if (!Linker_ReadMaterialSource(data, size, &source, error, errorSize))
    {
        return false;
    }
    LinkerMaterialTexture *result =
        source->textureCount ? (LinkerMaterialTexture *)calloc(source->textureCount, sizeof(LinkerMaterialTexture))
                             : NULL;
    bool valid = !source->textureCount || result;
    for (unsigned int i = 0; valid && i < source->textureCount; ++i)
    {
        MaterialTextureDefRaw raw;
        memcpy(&raw, (const uint8_t *)source + source->textureTableOffset + i * sizeof(MaterialTextureDefRaw),
               sizeof(MaterialTextureDefRaw));
        LinkerMaterialTexture *texture = &result[i];
        const char *name = (const char *)source + raw.nameOffset;
        const size_t length = strlen(name);
        if (length >= sizeof(texture->name))
        {
            valid = false;
            break;
        }
        memcpy(texture->name, name, length + 1);
        for (const char *p = name; *p; ++p)
        {
            texture->nameHash = (uint32_t)((int)*p | 0x20) ^ (33 * texture->nameHash);
        }
        texture->samplerState = raw.samplerState;
        texture->semantic = raw.semantic;
        if (raw.semantic == TS_WATER_MAP)
        {
            memcpy(&texture->waterWidth, (const uint8_t *)source + raw.u.waterDefOffset, sizeof(int32_t));
            memcpy(texture->waterParameters, (const uint8_t *)source + raw.u.waterDefOffset + sizeof(int32_t),
                   sizeof(float[6]));
        }
        else
        {
            const char *image = (const char *)source + raw.u.imageNameOffset;
            const size_t imageLength = strlen(image);
            if (imageLength >= sizeof(texture->image))
            {
                valid = false;
                break;
            }
            memcpy(texture->image, image, imageLength + 1);
        }
    }
    if (!valid)
    {
        free(result);
        free(source);
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid or unallocatable material texture dependencies");
        }
        return false;
    }
    *textures = result;
    *count = source->textureCount;
    free(source);
    return true;
}

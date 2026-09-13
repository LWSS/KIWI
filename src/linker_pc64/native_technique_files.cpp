#include <universal/q_shared.h>
#include <gfx_d3d/r_material.h>
#include <database64/db_package.h>
#include "native_asset_files.h"
#include "native_technique.h"
#include "native_shader.h"
#include "native_technique_files.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void Linker_FreeCompiledTechniqueSet(LinkerCompiledTechniqueSet *compiled)
{
    if (!compiled)
    {
        return;
    }
    for (unsigned int i = 0; i < 34; ++i)
    {
        LinkerCompiledTechnique *technique = compiled->techniques[i];
        for (unsigned int j = i + 1; j < 34; ++j)
        {
            if (compiled->techniques[j] == technique)
            {
                compiled->techniques[j] = NULL;
            }
        }
        Linker_FreeCompiledTechnique(technique);
    }
    free(compiled->techniqueSet);
    free(compiled);
}

void Linker_FreeCompiledTechnique(LinkerCompiledTechnique *compiled)
{
    if (!compiled)
    {
        return;
    }
    if (compiled->technique)
    {
        for (unsigned int i = 0; i < compiled->technique->passCount; ++i)
        {
            MaterialPass *pass = &compiled->technique->passArray[i];
            free(pass->vertexShader);
            free(pass->pixelShader);
            free(pass->vertexDecl);
            free(pass->args);
        }
        free(compiled->technique);
    }
    free(compiled);
}

bool Linker_CompileTechniqueSetFiles(const char *root, const char *name, const unsigned int *referenceBits,
                                     unsigned int toolFlags, LinkerCompiledTechniqueSet **compiled, char *error,
                                     size_t errorSize)
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
    char normalized[256], relative[512];
    if (!root || !name || !referenceBits || !DB64_NormalizePath(name, normalized, sizeof(normalized)))
    {
        return false;
    }
    snprintf(relative, sizeof(relative), "techsets/%s.techset", normalized);
    void *data = NULL;
    size_t size = 0;
    LinkerTechniqueSetSource source = {};
    bool valid = Linker_ReadRawAssetFile(root, relative, &data, &size) &&
                 Linker_ReadTechniqueSet(data, size, &source, error, errorSize);
    free(data);
    LinkerCompiledTechniqueSet *result =
        valid ? (LinkerCompiledTechniqueSet *)calloc(1, sizeof(LinkerCompiledTechniqueSet)) : NULL;
    valid = valid && result;
    if (valid)
    {
        result->techniqueSet = (MaterialTechniqueSet *)calloc(1, sizeof(MaterialTechniqueSet) + strlen(normalized) + 1);
        valid = result->techniqueSet != NULL;
    }
    if (valid)
    {
        result->techniqueSet->name = (char *)(result->techniqueSet + 1);
        strcpy((char *)result->techniqueSet->name, normalized);
        result->techniqueSet->remappedTechniqueSet = result->techniqueSet;
    }
    for (unsigned int i = 0; valid && i < 34; ++i)
    {
        // Match the game loader's g_useTechnique: these slots are Radiant-only.
        if ((i >= TECHNIQUE_FAKELIGHT_NORMAL && i <= TECHNIQUE_CASE_TEXTURE) || i == TECHNIQUE_WIREFRAME_SHADED ||
            !source.techniques[i][0])
        {
            continue;
        }
        for (unsigned int j = 0; j < i; ++j)
        {
            if (result->techniques[j] && !strcmp(source.techniques[i], source.techniques[j]))
            {
                result->techniques[i] = result->techniques[j];
                break;
            }
        }
        if (!result->techniques[i])
        {
            valid = Linker_CompileTechniqueFiles(root, source.techniques[i], referenceBits, toolFlags,
                                                 &result->techniques[i], error, errorSize);
        }
        if (valid)
        {
            result->techniqueSet->techniques[i] = result->techniques[i]->technique;
        }
    }
    if (!valid)
    {
        Linker_FreeCompiledTechniqueSet(result);
        if (errorSize && !error[0])
        {
            snprintf(error, errorSize, "Cannot compile technique set %s", normalized);
        }
        return false;
    }
    *compiled = result;
    return true;
}

bool Linker_CompileTechniqueFiles(const char *root, const char *name, const unsigned int *referenceBits,
                                  unsigned int toolFlags, LinkerCompiledTechnique **compiled, char *error,
                                  size_t errorSize)
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
    if (!root || !name || !referenceBits)
    {
        return false;
    }
    char normalized[256], relative[2048];
    if (!DB64_NormalizePath(name, normalized, sizeof(normalized)))
    {
        return false;
    }
    snprintf(relative, sizeof(relative), "techniques/%s.tech", normalized);
    void *data = NULL, *names = NULL;
    size_t size, namesSize;
    LinkerTechniqueSource *source = NULL;
    bool valid = Linker_ReadRawAssetFile(root, relative, &data, &size) &&
                 Linker_ReadTechnique(data, size, &source, error, errorSize);
    free(data);
    data = NULL;
    valid = valid && Linker_ReadRawAssetFile(root, "shader_bin/shader_names", &names, &namesSize);
    LinkerCompiledTechnique *result =
        valid ? (LinkerCompiledTechnique *)calloc(1, sizeof(LinkerCompiledTechnique)) : NULL;
    valid = valid && result;
    if (valid)
    {
        const size_t headerSize = offsetof(MaterialTechnique, passArray) + source->passCount * sizeof(MaterialPass);
        result->technique = (MaterialTechnique *)calloc(1, headerSize + strlen(normalized) + 1);
        valid = result->technique != NULL;
        if (valid)
        {
            result->technique->name = (char *)result->technique + headerSize;
            strcpy((char *)result->technique->name, normalized);
            result->technique->passCount = (uint16_t)source->passCount;
        }
    }
    for (unsigned int i = 0; valid && i < source->passCount; ++i)
    {
        const LinkerTechniquePassSource *input = &source->passes[i];
        MaterialVertexShader *vertex = NULL;
        MaterialPixelShader *pixel = NULL;
        for (int stage = 0; valid && stage < 2; ++stage)
        {
            char binary[128];
            const char *shaderName = stage ? input->pixelShader : input->vertexShader;
            valid = Linker_FindShaderBinary(names, namesSize, shaderName, stage != 0, binary, sizeof(binary), error,
                                            errorSize);
            if (valid)
            {
                snprintf(relative, sizeof(relative), "shader_bin/%s", binary);
                valid = Linker_ReadRawAssetFile(root, relative, &data, &size);
            }
            if (valid)
            {
                valid = stage ? Linker_ImportPixelShader(data, size, shaderName, &pixel, error, errorSize)
                              : Linker_ImportVertexShader(data, size, shaderName, &vertex, error, errorSize);
            }
            free(data);
            data = NULL;
        }
        MaterialPass *pass = &result->technique->passArray[i];
        valid = valid && Linker_BuildTechniquePass(source, i, vertex, pixel, pass, error, errorSize);
        if (!valid)
        {
            free(vertex);
            free(pixel);
            break;
        }
        result->technique->flags |= Linker_TechniquePassFlags(pass);
        snprintf(relative, sizeof(relative), "statemaps/%s.sm", input->stateMap);
        valid = Linker_ReadRawAssetFile(root, relative, &data, &size) &&
                Linker_ApplyStateMap(data, size, referenceBits, toolFlags, result->stateBits[i], error, errorSize);
        free(data);
        data = NULL;
    }
    free(names);
    if (source)
    {
        free(source->text);
        free(source);
    }
    if (!valid)
    {
        Linker_FreeCompiledTechnique(result);
        if (errorSize)
        {
            char detail[512];
            snprintf(detail, sizeof(detail), "%s", error[0] ? error : "Cannot read or compile technique files");
            snprintf(error, errorSize, "%s: %s", normalized, detail);
        }
        return false;
    }
    *compiled = result;
    return true;
}

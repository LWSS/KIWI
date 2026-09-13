#include <universal/q_shared.h>
#include <gfx_d3d/r_material.h>
#include "native_material_sort.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct SortLiteral
{
    unsigned int dest;
    float value[4];
};

struct MaterialSortEntry
{
    Material *material;
    unsigned int slot, prepass, codeCount, literalCount;
    bool lit, emissive, lightmap, writesDepth;
    const char *pixelShader;
    char vertexShader[128];
    uint16_t codes[255];
    SortLiteral literals[16];
};

static int CompareValue(unsigned int a, unsigned int b)
{
    return (a > b) - (a < b);
}

static int CompareLiteral(const void *a, const void *b)
{
    return CompareValue(((const SortLiteral *)a)->dest, ((const SortLiteral *)b)->dest);
}

static bool BuildSortEntry(Material *material, unsigned int slot, MaterialSortEntry *entry)
{
    if (!material || !material->info.name || !material->techniqueSet || !material->techniqueSet->name ||
        material->info.sortKey >= 64 || (material->constantCount && !material->constantTable))
    {
        return false;
    }
    MaterialTechniqueSet *set = material->techniqueSet;
    entry->material = material;
    entry->slot = slot;
    entry->lit = set->techniques[TECHNIQUE_LIT_BEGIN] != NULL;
    entry->emissive = set->techniques[TECHNIQUE_EMISSIVE] != NULL;
    entry->lightmap = (material->info.gameFlags & 2) != 0;
    entry->writesDepth = (material->stateFlags & 8) != 0;
    const MaterialTechnique *prepass = set->techniques[TECHNIQUE_DEPTH_PREPASS];
    entry->prepass = prepass ? ((material->stateFlags & 4) ? 3 : ((prepass->flags & 4) == 0))
                             : (set->techniques[TECHNIQUE_BUILD_FLOAT_Z] ? 2 : 3);
    const MaterialTechnique *technique = set->techniques[entry->lit ? TECHNIQUE_LIT_BEGIN : TECHNIQUE_EMISSIVE];
    if (!technique)
    {
        return true;
    }
    const MaterialPass *pass = technique->passArray;
    if (!technique->passCount || !pass->pixelShader || !pass->pixelShader->name || !pass->vertexShader ||
        !pass->vertexShader->name ||
        ((pass->perPrimArgCount || pass->perObjArgCount || pass->stableArgCount) && !pass->args))
    {
        return false;
    }
    entry->pixelShader = pass->pixelShader->name;
    strncpy(entry->vertexShader, pass->vertexShader->name, sizeof(entry->vertexShader) - 1);
    if (entry->lit && !entry->writesDepth)
    {
        return true;
    }
    for (unsigned int i = 0; i < pass->stableArgCount; ++i)
    {
        const MaterialShaderArgument *arg = &pass->args[pass->perPrimArgCount + pass->perObjArgCount + i];
        if (arg->type == MTL_ARG_CODE_PIXEL_CONST)
        {
            entry->codes[entry->codeCount++] = arg->u.codeConst.index;
        }
        else if (arg->type == MTL_ARG_MATERIAL_PIXEL_CONST || arg->type == MTL_ARG_LITERAL_PIXEL_CONST)
        {
            const float *value = NULL;
            if (arg->type == MTL_ARG_LITERAL_PIXEL_CONST)
            {
                value = arg->u.literalConst;
            }
            else
            {
                for (unsigned int j = 0; j < material->constantCount; ++j)
                {
                    if (material->constantTable[j].nameHash == arg->u.nameHash)
                    {
                        value = material->constantTable[j].literal;
                        break;
                    }
                }
            }
            if (!value || entry->literalCount == ARRAY_COUNT(entry->literals))
            {
                return false;
            }
            SortLiteral *literal = &entry->literals[entry->literalCount++];
            literal->dest = arg->dest;
            for (unsigned int j = 0; j < 4; ++j)
            {
                if (!isfinite(value[j]))
                {
                    return false;
                }
                literal->value[j] = value[j];
            }
        }
    }
    if (entry->literalCount > 1)
    {
        qsort(entry->literals, entry->literalCount, sizeof(SortLiteral), CompareLiteral);
        for (unsigned int i = 1; i < entry->literalCount; ++i)
        {
            if (entry->literals[i - 1].dest == entry->literals[i].dest)
            {
                return false;
            }
        }
    }
    return true;
}

static int ComparePixelConstants(const MaterialSortEntry *a, const MaterialSortEntry *b)
{
    int result = CompareValue(a->codeCount, b->codeCount);
    for (unsigned int i = 0; !result && i < a->codeCount; ++i)
    {
        result = CompareValue(a->codes[i], b->codes[i]);
    }
    if (!result)
    {
        result = CompareValue(a->literalCount, b->literalCount);
    }
    for (unsigned int i = 0; !result && i < a->literalCount; ++i)
    {
        result = CompareValue(a->literals[i].dest, b->literals[i].dest);
        for (unsigned int j = 0; !result && j < 4; ++j)
        {
            const float av = a->literals[i].value[j], bv = b->literals[i].value[j];
            result = (av > bv) - (av < bv);
        }
    }
    return result;
}

static int CompareMaterials(const void *left, const void *right)
{
    const MaterialSortEntry *a = (const MaterialSortEntry *)left;
    const MaterialSortEntry *b = (const MaterialSortEntry *)right;
    if (a->material == b->material)
    {
        return 0;
    }
    int result = CompareValue(b->lit, a->lit);
    if (!result && !a->lit)
    {
        result = CompareValue(b->emissive, a->emissive);
    }
    if (!result)
    {
        result = CompareValue(a->material->info.sortKey, b->material->info.sortKey);
    }
    if (!result && a->lit)
    {
        result = CompareValue(b->lightmap, a->lightmap);
    }
    if (!result)
    {
        result = CompareValue(a->prepass, b->prepass);
    }
    if (!result)
    {
        result = CompareValue(b->writesDepth, a->writesDepth);
    }
    if (!result && (a->lit || a->emissive))
    {
        result = strcmp(a->pixelShader, b->pixelShader);
        if (!result && (!a->lit || a->writesDepth))
        {
            result = ComparePixelConstants(a, b);
        }
        if (!result)
        {
            result = strcmp(a->vertexShader, b->vertexShader);
        }
    }
    if (!result)
    {
        result = strcmp(a->material->techniqueSet->name, b->material->techniqueSet->name);
    }
    if (!result)
    {
        result = strcmp(a->material->info.name, b->material->info.name);
    }
    return result;
}

bool Linker_BuildMaterialRanks(Material *const *materials, unsigned int count, uint16_t **ranks, char *error,
                               size_t errorSize)
{
    if (!ranks || (!error && errorSize))
    {
        return false;
    }
    *ranks = NULL;
    if (errorSize)
    {
        error[0] = 0;
    }
    bool valid = count <= 65536 && (!count || materials);
    MaterialSortEntry *entries = valid && count ? (MaterialSortEntry *)calloc(count, sizeof(MaterialSortEntry)) : NULL;
    uint16_t *result = valid && count ? (uint16_t *)calloc(count, sizeof(uint16_t)) : NULL;
    valid = valid && (!count || (entries && result));
    unsigned int used = 0;
    for (unsigned int i = 0; valid && i < count; ++i)
    {
        if (materials[i])
        {
            valid = BuildSortEntry(materials[i], i, &entries[used++]);
            if (!valid && errorSize)
            {
                snprintf(error, errorSize, "Cannot order material '%s': invalid shader or constant data",
                         materials[i]->info.name ? materials[i]->info.name : "");
            }
        }
    }
    if (valid)
    {
        if (used > 1)
        {
            qsort(entries, used, sizeof(MaterialSortEntry), CompareMaterials);
        }
        unsigned int rank = 0;
        for (unsigned int i = 0; i < used; ++i)
        {
            if (i && entries[i].material != entries[i - 1].material)
            {
                if (!CompareMaterials(&entries[i - 1], &entries[i]))
                {
                    valid = false;
                    break;
                }
                ++rank;
            }
            result[entries[i].slot] = (uint16_t)rank;
        }
    }
    free(entries);
    if (!valid)
    {
        free(result);
        if (errorSize && !error[0])
        {
            snprintf(error, errorSize, "Cannot order materials: invalid count, duplicate asset or allocation");
        }
        return false;
    }
    *ranks = result;
    return true;
}

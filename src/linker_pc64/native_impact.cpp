#include <universal/q_shared.h>
#include <gfx_d3d/fxprimitives.h>
#include <database64/db_package.h>
#include "native_impact.h"
#include "native_asset_files.h"
#include "native_stringtable.h"
#include <stdlib.h>
#include <stdio.h>

static const char *impactTypes[12] = {"bullet_small_normal", "bullet_small_exit", "bullet_large_normal",
    "bullet_large_exit", "shotgun_normal", "shotgun_exit", "bullet_ap_normal", "bullet_ap_exit",
    "grenade_bounce", "grenade_explode", "rocket_explode", "projectile_dud"};
static const char *impactSurfaces[33] = {"default", "bark", "brick", "carpet", "cloth", "concrete", "dirt",
    "flesh", "foliage", "glass", "grass", "gravel", "ice", "metal", "mud", "paper", "plaster", "rock",
    "sand", "snow", "water", "wood", "asphalt", "ceramic", "plastic", "rubber", "cushion", "fruit",
    "paintedmetal", "flesh_body_nonfatal", "flesh_body_fatal", "flesh_head_nonfatal", "flesh_head_fatal"};

struct ImpactInput
{
    char names[12][33][64];
    bool present[12][33];
    char *paths[4096];
    int pathCount;
    size_t bytes;
};
static bool Gather(const char *path, void *context)
{
    ImpactInput *input = (ImpactInput *)context;
    for (int i = 0; i < input->pathCount; ++i)
    {
        if (!_stricmp(path, input->paths[i]))
        {
            return true;
        }
    }
    if (input->pathCount == ARRAY_COUNT(input->paths))
    {
        return false;
    }
    char *copy = _strdup(path);
    if (!copy)
    {
        return false;
    }
    input->paths[input->pathCount++] = copy;
    return true;
}
static int Compare(const void *a, const void *b)
{
    return _stricmp(*(const char *const *)a, *(const char *const *)b);
}
static bool ReadDirectory(ImpactInput *input, const char *root, const char *directory, char *error, size_t errorSize)
{
    bool ok = Linker_EnumerateRawAssetFiles(root, directory, ".csv", Gather, input);
    qsort(input->paths, input->pathCount, sizeof(char *), Compare);
    for (int i = 0; ok && i < input->pathCount; ++i)
    {
        void *data = NULL;
        size_t size = 0;
        StringTable *table = NULL;
        ok = Linker_ReadRawAssetFile(root, input->paths[i], &data, &size) &&
             (input->bytes += size) <= 64 * 1024 * 1024 &&
             Linker_ImportStringTable(data, size, input->paths[i], &table, error, errorSize);
        free(data);
        for (int row = 0; ok && row < table->rowCount; ++row)
        {
            const char *type = table->values[row * table->columnCount];
            if (!type[0] || type[0] == '#')
            {
                continue;
            }
            if (table->columnCount < 2)
            {
                ok = false;
                break;
            }
            const char *surface = table->values[row * table->columnCount + 1];
            const char *effect = table->columnCount > 2 ? table->values[row * table->columnCount + 2] : "";
            if (!_stricmp(surface, "foilage"))
            {
                surface = "foliage";
            }
            int t = 0, s = 0;
            while (t < 12 && _stricmp(type, impactTypes[t]))
            {
                ++t;
            }
            while (s < 33 && _stricmp(surface, impactSurfaces[s]))
            {
                ++s;
            }
            if (t == 12 || s == 33 || strlen(effect) >= 64)
            {
                ok = false;
                break;
            }
            strcpy(input->names[t][s], effect);
            input->present[t][s] = true;
        }
        free(table);
        if (!ok && errorSize && !error[0])
        {
            snprintf(error, errorSize, "Invalid impact-effect CSV: %s", input->paths[i]);
        }
    }
    for (int i = 0; i < input->pathCount; ++i)
    {
        free(input->paths[i]);
    }
    input->pathCount = 0;
    return ok;
}

bool Linker_CompileImpactFiles(const char *root, const char *map, LinkerImpactEffect resolve, void *context,
                              FxImpactTable **result, char *error, size_t errorSize)
{
    if (!result || (!error && errorSize))
    {
        return false;
    }
    *result = NULL;
    if (errorSize)
    {
        error[0] = 0;
    }
    if (!root || !map || !DB64_ValidMapName(map))
    {
        return false;
    }
    ImpactInput *input = (ImpactInput *)calloc(1, sizeof(ImpactInput));
    FxImpactTable *output = (FxImpactTable *)calloc(1, sizeof(FxImpactTable) + 12 * sizeof(FxImpactEntry));
    bool ok = input && output;
    char directory[96];
    snprintf(directory, sizeof(directory), "fx/maps/%s", map);
    ok = ok && ReadDirectory(input, root, "fx", error, errorSize) &&
         ReadDirectory(input, root, directory, error, errorSize);
    if (ok)
    {
        output->name = "";
        output->table = (FxImpactEntry *)(output + 1);
    }
    for (int t = 0; ok && t < 12; ++t)
    {
        for (int s = 0; ok && s < 33; ++s)
        {
            const int source = s >= 29 && (!input->present[t][s] || !input->names[t][s][0]) ? 7 : s;
            if (!input->present[t][source])
            {
                if (errorSize)
                {
                    snprintf(error, errorSize, "Missing impact-effect entry: %s, %s", impactTypes[t], impactSurfaces[s]);
                }
                ok = false;
                break;
            }
            const char *name = input->names[t][source];
            const FxEffectDef *effect = name[0] && resolve ? resolve(name, context) : NULL;
            if (name[0] && !effect)
            {
                ok = false;
                break;
            }
            if (s < 29)
            {
                output->table[t].nonflesh[s] = effect;
            }
            else
            {
                output->table[t].flesh[s - 29] = effect;
            }
        }
    }
    free(input);
    if (!ok)
    {
        free(output);
        if (errorSize && !error[0])
        {
            snprintf(error, errorSize, "Cannot compile impact effects for '%s'", map);
        }
        return false;
    }
    *result = output;
    return true;
}

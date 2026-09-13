#include "db_external_assets.h"
#include <universal/q_shared.h>
#include <gfx_d3d/r_image.h>
#include "database.h"
#include "db_material_assets.h"
#include "db_image_assets.h"
#include "db_technique_assets.h"
#include <limits.h>

static void LoadArray(void **array, size_t size)
{
    if ((uintptr_t)*array == UINTPTR_MAX)
    {
        *array = DB_AllocStreamPos(15);
        Load_Stream(true, (uint8_t *)*array, size);
    }
    else if (*array)
    {
        DB64_ConvertOffsetRange((uintptr_t *)array, size);
    }
}

static void LoadWater(water_t **water)
{
    if ((uintptr_t)*water == UINTPTR_MAX)
    {
        *water = (water_t *)DB_AllocStreamPos(15);
        water_t *value = *water;
        Load_Stream(true, (uint8_t *)value, sizeof(water_t));
        if (value->M <= 0 || value->N <= 0 || (value->M & (value->M - 1)) || (value->N & (value->N - 1)) ||
            (size_t)value->M > INT_MAX / sizeof(complex_s) / (size_t)value->N || !value->H0 || !value->wTerm ||
            !value->image)
        {
            Com_Error(ERR_DROP, "Invalid native water grid");
        }
        const size_t count = (size_t)value->M * value->N;
        value->H0 = (complex_s *)DB_AllocStreamPos(15);
        Load_Stream(true, (uint8_t *)value->H0, count * sizeof(complex_s));
        value->wTerm = (float *)DB_AllocStreamPos(15);
        Load_Stream(true, (uint8_t *)value->wTerm, count * sizeof(float));
        DB64_LoadImageAsset((XAssetHeader *)&value->image, false);
    }
    else if (*water)
    {
        DB64_ConvertOffsetRange((uintptr_t *)water, sizeof(water_t));
    }
}

static void LoadMaterial(Material *material)
{
    Load_Stream(true, (uint8_t *)material, sizeof(Material));
    if (!material->info.name)
    {
        Com_Error(ERR_DROP, "Native material has no name");
    }
    DB_PushStreamPos(4);
    DB64_LoadAssetString(&material->info.name);
    if (material->info.name[0] == ',')
    {
        if (!material->info.name[1] || material->techniqueSet || material->textureTable ||
            material->constantTable || material->stateBitsTable || material->textureCount ||
            material->constantCount || material->stateBitsCount)
        {
            Com_Error(ERR_DROP, "Invalid native material reference");
        }
        DB_PopStreamPos();
        return;
    }
    if (!material->info.name || !material->techniqueSet ||
        (!!material->textureTable != (material->textureCount != 0)) ||
        (!!material->constantTable != (material->constantCount != 0)) ||
        (!!material->stateBitsTable != (material->stateBitsCount != 0)))
    {
        Com_Error(ERR_DROP, "Invalid native material metadata");
    }
    for (uint i = 0; i < ARRAY_COUNT(material->stateBitsEntry); ++i)
    {
        if (material->stateBitsEntry[i] != 255 && material->stateBitsEntry[i] >= material->stateBitsCount)
        {
            Com_Error(ERR_DROP, "Native material state entry exceeds its table");
        }
    }
    DB64_LoadTechniqueSet((XAssetHeader *)&material->techniqueSet, false);
    if (material->textureTable)
    {
        const bool inlineTable = (uintptr_t)material->textureTable == UINTPTR_MAX;
        LoadArray((void **)&material->textureTable, material->textureCount * sizeof(MaterialTextureDef));
        if (inlineTable)
        {
            for (uint i = 0; i < material->textureCount; ++i)
            {
                MaterialTextureDef *texture = &material->textureTable[i];
                if (texture->semantic == TS_WATER_MAP)
                {
                    LoadWater(&texture->u.water);
                }
                else
                {
                    DB64_LoadImageAsset((XAssetHeader *)&texture->u.image, false);
                }
            }
        }
    }
    LoadArray((void **)&material->constantTable, material->constantCount * sizeof(MaterialConstantDef));
    LoadArray((void **)&material->stateBitsTable, material->stateBitsCount * sizeof(GfxStateBits));
    DB_PopStreamPos();
}

void DB64_LoadMaterialAsset(XAssetHeader *header, bool atStreamStart)
{
    Load_Stream(atStreamStart, (uint8_t *)header, sizeof(XAssetHeader));
    if (DB64_LoadExternalAsset(ASSET_TYPE_MATERIAL, header))
    {
        return;
    }
    DB_PushStreamPos(0);
    const uintptr_t token = (uintptr_t)header->data;
    if (token)
    {
        if (token == UINTPTR_MAX || token == UINTPTR_MAX - 1)
        {
            header->material = (Material *)DB_AllocStreamPos(15);
            const void **inserted = token == UINTPTR_MAX - 1 ? DB_InsertPointer() : NULL;
            LoadMaterial(header->material);
            Load_MaterialAsset(header);
            if (inserted)
            {
                *inserted = header->data;
            }
        }
        else
        {
            DB_ConvertOffsetToAlias((uintptr_t *)header);
        }
    }
    DB_PopStreamPos();
}

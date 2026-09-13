#include "db_external_assets.h"
#include <universal/q_shared.h>
#include <gfx_d3d/r_image.h>
#include "database.h"
#include "db_image_assets.h"

static void LoadImage(GfxImage *image)
{
    Load_Stream(true, (uint8_t *)image, sizeof(GfxImage));
    if (!image->name || !image->texture.loadDef || *((uint8_t *)image + offsetof(GfxImage, noPicmip)) > 1 ||
        *((uint8_t *)image + offsetof(GfxImage, delayLoadPixels)) > 1)
    {
        Com_Error(ERR_DROP, "Invalid native image metadata");
    }
    DB_PushStreamPos(4);
    DB64_LoadAssetString(&image->name);
    DB_PushStreamPos(0);
    const uintptr_t token = (uintptr_t)image->texture.loadDef;
    if (token == UINTPTR_MAX || token == UINTPTR_MAX - 1)
    {
        image->texture.loadDef = (GfxImageLoadDef *)DB_AllocStreamPos(15);
        const void **inserted = token == UINTPTR_MAX - 1 ? DB_InsertPointer() : NULL;
        GfxImageLoadDef *definition = image->texture.loadDef;
        Load_Stream(true, (uint8_t *)definition, offsetof(GfxImageLoadDef, data));
        if (!DB64_ValidateImageLayout(image, definition))
        {
            Com_Error(ERR_DROP, "Invalid native image mip layout: %s", image->name);
        }
        Load_Stream(true, definition->data, definition->resourceSize);
        Load_Texture(&image->texture, image);
        if (inserted)
        {
            *inserted = image->texture.basemap;
        }
    }
    else
    {
        DB_ConvertOffsetToAlias((uintptr_t *)&image->texture);
    }
    DB_PopStreamPos();
    DB_PopStreamPos();
}

void DB64_LoadImageAsset(XAssetHeader *header, bool atStreamStart)
{
    Load_Stream(atStreamStart, (uint8_t *)header, sizeof(XAssetHeader));
    if (DB64_LoadExternalAsset(ASSET_TYPE_IMAGE, header))
    {
        return;
    }
    DB_PushStreamPos(0);
    const uintptr_t token = (uintptr_t)header->data;
    if (token)
    {
        if (token == UINTPTR_MAX || token == UINTPTR_MAX - 1)
        {
            header->image = (GfxImage *)DB_AllocStreamPos(15);
            const void **inserted = token == UINTPTR_MAX - 1 ? DB_InsertPointer() : NULL;
            LoadImage(header->image);
            Load_GfxImageAsset(header);
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

void DB64_LoadLightDefAsset(XAssetHeader *header, bool atStreamStart)
{
    Load_Stream(atStreamStart, (uint8_t *)header, sizeof(XAssetHeader));
    if (DB64_LoadExternalAsset(ASSET_TYPE_LIGHT_DEF, header))
    {
        return;
    }
    DB_PushStreamPos(0);
    const uintptr_t token = (uintptr_t)header->data;
    if (token)
    {
        if (token == UINTPTR_MAX || token == UINTPTR_MAX - 1)
        {
            header->lightDef = (GfxLightDef *)DB_AllocStreamPos(15);
            const void **inserted = token == UINTPTR_MAX - 1 ? DB_InsertPointer() : NULL;
            GfxLightDef *definition = header->lightDef;
            Load_Stream(true, (uint8_t *)definition, sizeof(GfxLightDef));
            if (!definition->name || !definition->attenuation.image || definition->lmapLookupStart < 1 ||
                definition->lmapLookupStart >= 512)
            {
                Com_Error(ERR_DROP, "Invalid native light definition");
            }
            DB_PushStreamPos(4);
            DB64_LoadAssetString(&definition->name);
            DB64_LoadImageAsset((XAssetHeader *)&definition->attenuation.image, false);
            if (definition->attenuation.image->width + definition->lmapLookupStart >= 512)
            {
                Com_Error(ERR_DROP, "Native light falloff exceeds its lightmap row");
            }
            DB_PopStreamPos();
            Load_LightDefAsset(header);
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

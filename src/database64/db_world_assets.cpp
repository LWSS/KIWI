#include "db_external_assets.h"
#include <universal/q_shared.h>
#include <game/g_bsp.h>
#include "database.h"
#include "db_world_assets.h"
#include <limits.h>

static void LoadEntities(MapEnts *entities)
{
    Load_Stream(true, (uint8_t *)entities, sizeof(MapEnts));
    if (!entities->name || !entities->entityString || entities->numEntityChars <= 0)
    {
        Com_Error(ERR_DROP, "Invalid native map entities");
    }
    DB_PushStreamPos(4);
    DB64_LoadAssetString(&entities->name);
    entities->entityString = (char *)DB_GetStreamPos();
    Load_Stream(true, (uint8_t *)entities->entityString, entities->numEntityChars);
    if (entities->entityString[entities->numEntityChars - 1])
    {
        Com_Error(ERR_DROP, "Unterminated native map entities");
    }
    DB_PopStreamPos();
}

static void LoadCommonWorld(ComWorld *world)
{
    Load_Stream(true, (uint8_t *)world, sizeof(ComWorld));
    if (!world->name || world->primaryLightCount > 255 ||
        (!!world->primaryLights != (world->primaryLightCount != 0)))
    {
        Com_Error(ERR_DROP, "Invalid native common world");
    }
    DB_PushStreamPos(4);
    DB64_LoadAssetString(&world->name);
    if (world->primaryLights)
    {
        world->primaryLights = (ComPrimaryLight *)DB_AllocStreamPos(15);
        Load_Stream(true, (uint8_t *)world->primaryLights, world->primaryLightCount * sizeof(ComPrimaryLight));
        for (uint i = 0; i < world->primaryLightCount; ++i)
        {
            DB64_LoadAssetString(&world->primaryLights[i].defName);
        }
    }
    DB_PopStreamPos();
}

static void LoadGameWorld(GameWorldMp *world)
{
    Load_Stream(true, (uint8_t *)world, sizeof(GameWorldMp));
    if (!world->name)
    {
        Com_Error(ERR_DROP, "Invalid native multiplayer world");
    }
    DB_PushStreamPos(4);
    DB64_LoadAssetString(&world->name);
    DB_PopStreamPos();
}

void DB64_LoadWorldAsset(XAssetType type, XAssetHeader *header, bool atStreamStart)
{
    if (type != ASSET_TYPE_MAP_ENTS && type != ASSET_TYPE_COMWORLD && type != ASSET_TYPE_GAMEWORLD_MP)
    {
        Com_Error(ERR_DROP, "Invalid native world asset type");
    }
    Load_Stream(atStreamStart, (uint8_t *)header, sizeof(XAssetHeader));
    if (DB64_LoadExternalAsset(type, header))
    {
        return;
    }
    DB_PushStreamPos(0);
    const uintptr_t token = (uintptr_t)header->data;
    if (token)
    {
        if (token == UINTPTR_MAX || token == UINTPTR_MAX - 1)
        {
            header->data = DB_AllocStreamPos(15);
            const void **inserted = token == UINTPTR_MAX - 1 ? DB_InsertPointer() : NULL;
            switch (type)
            {
            case ASSET_TYPE_MAP_ENTS:
                LoadEntities(header->mapEnts);
                Load_MapEntsAsset(header);
                break;
            case ASSET_TYPE_COMWORLD:
                LoadCommonWorld(header->comWorld);
                Load_ComWorldAsset(header);
                break;
            case ASSET_TYPE_GAMEWORLD_MP:
                LoadGameWorld(header->gameWorldMp);
                Load_GameWorldMpAsset(header);
                break;
            }
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

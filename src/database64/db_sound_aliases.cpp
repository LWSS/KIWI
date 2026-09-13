#include "db_external_assets.h"
#include <universal/q_shared.h>
#include <universal/com_sndalias.h>
#include "database.h"
#include "db_sound_aliases.h"
#include "db_sound_assets.h"
#include "db_preset_assets.h"
#include <limits.h>

static bool LoadArray(void **array, size_t size)
{
    if ((uintptr_t)*array == UINTPTR_MAX)
    {
        *array = DB_AllocStreamPos(15);
        Load_Stream(true, (uint8_t *)*array, size);
        return true;
    }
    if (*array)
    {
        DB64_ConvertOffsetRange((uintptr_t *)array, size);
    }
    return false;
}

static void LoadAlias(snd_alias_t *alias)
{
    if (!alias->aliasName || !alias->soundFile)
    {
        Com_Error(ERR_DROP, "Native sound alias is missing its name or file");
    }
    DB64_LoadAssetString(&alias->aliasName);
    DB64_LoadAssetString(&alias->subtitle);
    DB64_LoadAssetString(&alias->secondaryAliasName);
    DB64_LoadAssetString(&alias->chainAliasName);
    if (LoadArray((void **)&alias->soundFile, sizeof(SoundFile)))
    {
        SoundFile *file = alias->soundFile;
        if (file->exists > 1 || (file->type != SAT_LOADED && file->type != SAT_STREAMED))
        {
            Com_Error(ERR_DROP, "Invalid native sound file type");
        }
        if (file->type == SAT_LOADED)
        {
            if (file->exists && !file->u.loadSnd)
            {
                Com_Error(ERR_DROP, "Native sound file marked present has no loaded sound");
            }
            DB64_LoadSoundAsset((XAssetHeader *)&file->u.loadSnd, false);
        }
        else
        {
            DB64_LoadAssetString(&file->u.streamSnd.filename.info.raw.dir);
            DB64_LoadAssetString(&file->u.streamSnd.filename.info.raw.name);
            if (!file->u.streamSnd.filename.info.raw.name)
            {
                Com_Error(ERR_DROP, "Native streamed sound has no filename");
            }
        }
    }
    DB64_LoadPresetAsset(ASSET_TYPE_SOUND_CURVE, (XAssetHeader *)&alias->volumeFalloffCurve, false);
    if (LoadArray((void **)&alias->speakerMap, sizeof(SpeakerMap)))
    {
        uint8_t isDefault;
        memcpy(&isDefault, &alias->speakerMap->isDefault, sizeof(uint8_t));
        if (isDefault > 1)
        {
            Com_Error(ERR_DROP, "Invalid native speaker map boolean");
        }
        DB64_LoadAssetString(&alias->speakerMap->name);
        char error[256];
        if (!DB64_ValidateSpeakerMap(alias->speakerMap, error, sizeof(error)))
        {
            Com_Error(ERR_DROP, "%s", error);
        }
    }
}

void DB64_LoadSoundAliases(XAssetHeader *header, bool atStreamStart)
{
    Load_Stream(atStreamStart, (uint8_t *)header, sizeof(XAssetHeader));
    if (DB64_LoadExternalAsset(ASSET_TYPE_SOUND, header))
    {
        return;
    }
    DB_PushStreamPos(0);
    const uintptr_t token = (uintptr_t)header->data;
    if (token == UINTPTR_MAX || token == UINTPTR_MAX - 1)
    {
        header->sound = (snd_alias_list_t *)DB_AllocStreamPos(15);
        const void **inserted = token == UINTPTR_MAX - 1 ? DB_InsertPointer() : NULL;
        snd_alias_list_t *list = header->sound;
        Load_Stream(true, (uint8_t *)list, sizeof(snd_alias_list_t));
        if (!list->aliasName || list->count < 0 || list->count > INT_MAX / sizeof(snd_alias_t) ||
            (!!list->head != (list->count != 0)))
        {
            Com_Error(ERR_DROP, "Invalid native sound alias list");
        }
        DB_PushStreamPos(4);
        DB64_LoadAssetString(&list->aliasName);
        if (LoadArray((void **)&list->head, list->count * sizeof(snd_alias_t)))
        {
            for (int i = 0; i < list->count; ++i)
            {
                LoadAlias(&list->head[i]);
            }
        }
        DB_PopStreamPos();
        Load_snd_alias_list_Asset(header);
        if (inserted)
        {
            *inserted = header->data;
        }
    }
    else if (token)
    {
        DB_ConvertOffsetToAlias((uintptr_t *)header);
    }
    DB_PopStreamPos();
}

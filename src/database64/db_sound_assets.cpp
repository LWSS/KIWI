#include "db_external_assets.h"
#include <universal/q_shared.h>
#include "database.h"
#include "db_sound_assets.h"

void DB64_LoadSoundAsset(XAssetHeader *header, bool atStreamStart)
{
    Load_Stream(atStreamStart, (uint8_t *)header, sizeof(XAssetHeader));
    if (DB64_LoadExternalAsset(ASSET_TYPE_LOADED_SOUND, header))
    {
        return;
    }
    DB_PushStreamPos(0);
    const uintptr_t token = (uintptr_t)header->data;
    if (token == UINTPTR_MAX || token == UINTPTR_MAX - 1)
    {
        header->loadSnd = (LoadedSound *)DB_AllocStreamPos(15);
        const void **inserted = token == UINTPTR_MAX - 1 ? DB_InsertPointer() : NULL;
        LoadedSound *sound = header->loadSnd;
        Load_Stream(true, (uint8_t *)sound, sizeof(LoadedSound));
        char error[256];
        if (!DB64_ValidateLoadedSound(sound, error, sizeof(error)))
        {
            Com_Error(ERR_DROP, "%s", error);
        }
        if (sound->sound.info.data_ptr || sound->sound.info.initial_ptr || (uintptr_t)sound->sound.data != UINTPTR_MAX)
        {
            Com_Error(ERR_DROP, "Native loaded sound contains runtime audio pointers");
        }
        DB_PushStreamPos(4);
        DB64_LoadAssetString(&sound->name);
        // SND_SetData copies into Miles-owned storage; source bytes are temporary.
        DB_PushStreamPos(0);
        uint8_t *audio = DB_AllocStreamPos(0);
        Load_Stream(true, audio, sound->sound.info.data_len);
        SND_SetData(&sound->sound, audio);
        DB_PopStreamPos();
        DB_PopStreamPos();
        Load_LoadedSoundAsset(header);
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

#include "db_external_assets.h"
#include <universal/q_shared.h>
#include "database.h"
#include "db_weapon_assets.h"
#include "db_weapon_layout.h"
#include "db_model_assets.h"
#include "db_material_assets.h"
#include "db_effect_assets.h"
#include "db_sound_aliases.h"
#include <math.h>

static void WeaponGraph(float (**graph)[2], int count)
{
    if (!*graph)
    {
        return;
    }
    if ((uintptr_t)*graph != UINTPTR_MAX)
    {
        Com_Error(ERR_DROP, "Invalid native weapon accuracy graph pointer");
    }
    *graph = (float (*)[2])DB_AllocStreamPos(15);
    Load_Stream(true, (uint8_t *)*graph, count * sizeof(float[2]));
    for (int i = 0; i < count; ++i)
    {
        if (!isfinite((*graph)[i][0]) || !isfinite((*graph)[i][1]))
        {
            Com_Error(ERR_DROP, "Invalid native weapon accuracy graph knot");
        }
    }
}

void DB64_LoadWeaponAsset(XAssetHeader *header, bool atStreamStart)
{
    Load_Stream(atStreamStart, (uint8_t *)header, sizeof(XAssetHeader));
    if (DB64_LoadExternalAsset(ASSET_TYPE_WEAPON, header))
    {
        return;
    }
    DB_PushStreamPos(0);
    const uintptr_t token = (uintptr_t)header->data;
    if (token == UINTPTR_MAX || token == UINTPTR_MAX - 1)
    {
        header->weapon = (WeaponDef *)DB_AllocStreamPos(15);
        const void **inserted = token == UINTPTR_MAX - 1 ? DB_InsertPointer() : NULL;
        WeaponDef *weapon = header->weapon;
        Load_Stream(true, (uint8_t *)weapon, sizeof(WeaponDef));
        if (!DB64_ValidateWeaponHeader(weapon))
        {
            Com_Error(ERR_DROP, "Invalid native weapon header");
        }
        DB_PushStreamPos(4);
        for (int i = 0; i < ARRAY_COUNT(db64WeaponFields); ++i)
        {
            const DB64WeaponField *field = &db64WeaponFields[i];
            for (int j = 0; j < field->count; ++j)
            {
                uint8_t *address = (uint8_t *)weapon + field->offset + j * sizeof(void *);
                // Load through the actual field in stream memory, so offset aliases remain valid.
                if (field->type == DB64_WEAPON_STRING)
                {
                    DB64_LoadAssetString((const char **)address);
                }
                else if (field->type == DB64_WEAPON_MODEL)
                {
                    DB64_LoadModelAsset((XAssetHeader *)address, false);
                }
                else if (field->type == DB64_WEAPON_MATERIAL)
                {
                    DB64_LoadMaterialAsset((XAssetHeader *)address, false);
                }
                else if (field->type == DB64_WEAPON_EFFECT)
                {
                    DB64_LoadEffectAsset((XAssetHeader *)address, false);
                }
                else
                {
                    DB64_LoadSoundAliases((XAssetHeader *)address, false);
                }
            }
        }
        if (!weapon->szInternalName[0])
        {
            Com_Error(ERR_DROP, "Native weapon has an empty name");
        }
        if (weapon->bounceSound)
        {
            if ((uintptr_t)weapon->bounceSound != UINTPTR_MAX)
            {
                Com_Error(ERR_DROP, "Invalid native weapon bounce-sound pointer");
            }
            weapon->bounceSound = (snd_alias_list_t **)DB_AllocStreamPos(15);
            Load_Stream(true, (uint8_t *)weapon->bounceSound, 29 * sizeof(snd_alias_list_t *));
            for (int i = 0; i < 29; ++i)
            {
                DB64_LoadSoundAliases((XAssetHeader *)&weapon->bounceSound[i], false);
            }
        }
        for (int i = 0; i < 2; ++i)
        {
            WeaponGraph(&weapon->accuracyGraphKnots[i], weapon->accuracyGraphKnotCount[i]);
            WeaponGraph(&weapon->originalAccuracyGraphKnots[i], weapon->originalAccuracyGraphKnotCount[i]);
        }
        uint16_t *tags[] = {weapon->hideTags, weapon->notetrackSoundMapKeys, weapon->notetrackSoundMapValues};
        const int counts[] = {8, 16, 16};
        for (int group = 0; group < 3; ++group)
        {
            for (int i = 0; i < counts[group]; ++i)
            {
                if (tags[group][i])
                {
                    Load_ScriptStringCustom(&tags[group][i]);
                }
            }
        }
        DB_PopStreamPos();
        Load_WeaponDefAsset(header);
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

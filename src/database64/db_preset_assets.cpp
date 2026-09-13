#include "db_external_assets.h"
#include <universal/q_shared.h>
#include <sound/snd_public.h>
#include "database.h"
#include "db_preset_assets.h"

static void LoadPhysics(PhysPreset *preset)
{
    Load_Stream(true, (uint8_t *)preset, sizeof(PhysPreset));
    if (!preset->name || *((uint8_t *)preset + offsetof(PhysPreset, tempDefaultToCylinder)) > 1)
    {
        Com_Error(ERR_DROP, "Invalid native physics preset (name %p, cylinder flag %d)", preset->name,
                  *((uint8_t *)preset + offsetof(PhysPreset, tempDefaultToCylinder)));
    }
    DB_PushStreamPos(4);
    DB64_LoadAssetString(&preset->name);
    DB64_LoadAssetString(&preset->sndAliasPrefix);
    DB_PopStreamPos();
}

static void LoadCurve(SndCurve *curve)
{
    Load_Stream(true, (uint8_t *)curve, sizeof(SndCurve));
    if (!curve->filename || curve->knotCount < 2 || curve->knotCount > ARRAY_COUNT(curve->knots))
    {
        Com_Error(ERR_DROP, "Invalid native sound curve");
    }
    DB_PushStreamPos(4);
    DB64_LoadAssetString(&curve->filename);
    DB_PopStreamPos();
}

void DB64_LoadPresetAsset(XAssetType type, XAssetHeader *header, bool atStreamStart)
{
    if (type != ASSET_TYPE_PHYSPRESET && type != ASSET_TYPE_SOUND_CURVE && type != ASSET_TYPE_SNDDRIVER_GLOBALS)
    {
        Com_Error(ERR_DROP, "Invalid native preset asset type");
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
            if (type == ASSET_TYPE_PHYSPRESET)
            {
                LoadPhysics(header->physPreset);
                Load_PhysPresetAsset(header);
            }
            else if (type == ASSET_TYPE_SNDDRIVER_GLOBALS)
            {
                SndDriverGlobals *globals = header->sndDriverGlobals;
                Load_Stream(true, (uint8_t *)globals, sizeof(SndDriverGlobals));
                if (!globals->name)
                {
                    Com_Error(ERR_DROP, "Invalid native sound driver globals");
                }
                DB_PushStreamPos(4);
                DB64_LoadAssetString(&globals->name);
                if (strcmp(globals->name, "singleton"))
                {
                    Com_Error(ERR_DROP, "Invalid native sound driver globals name");
                }
                DB_PopStreamPos();
                Load_SndDriverGlobalsAsset(header);
            }
            else
            {
                LoadCurve(header->sndCurve);
                Load_SndCurveAsset(header);
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

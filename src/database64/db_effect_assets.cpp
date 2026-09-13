#include "db_external_assets.h"
#include <universal/q_shared.h>
#include <gfx_d3d/fxprimitives.h>
#include "database.h"
#include "db_effect_assets.h"
#include "db_effect_layout.h"
#include "db_effect_references.h"
#include "db_material_assets.h"
#include "db_model_assets.h"

static void LoadEffectReference(FxEffectDefRef *reference)
{
    DB64_LoadAssetString(&reference->name);
    DB64_DeferEffectReference(reference);
}

static void LoadVisual(FxElemVisuals *visual, int type)
{
    switch (type)
    {
    case 5:
        DB64_LoadModelAsset((XAssetHeader *)&visual->model, false);
        break;
    case 6:
    case 7:
        if (visual->anonymous)
        {
            Com_Error(ERR_DROP, "Native light effect has a visual pointer");
        }
        break;
    case 8:
        DB64_LoadAssetString(&visual->soundName);
        break;
    case 10:
        LoadEffectReference(&visual->effectDef);
        break;
    default:
        DB64_LoadMaterialAsset((XAssetHeader *)&visual->material, false);
        break;
    }
}

static void LoadElement(FxElemDef *element)
{
    if (!DB64_ValidateEffectElement(element))
    {
        Com_Error(ERR_DROP, "Invalid native effect element metadata");
    }
    DB64_LoadEffectSamples(element);
    if (element->elemType == 9)
    {
        const size_t bytes = element->visualCount * sizeof(FxElemMarkVisuals);
        if ((uintptr_t)element->visuals.markArray == UINTPTR_MAX)
        {
            element->visuals.markArray = (FxElemMarkVisuals *)DB_AllocStreamPos(15);
            Load_Stream(true, (uint8_t *)element->visuals.markArray, bytes);
            for (int i = 0; i < element->visualCount; ++i)
            {
                for (int j = 0; j < 2; ++j)
                {
                    DB64_LoadMaterialAsset((XAssetHeader *)&element->visuals.markArray[i].materials[j], false);
                }
            }
        }
        else if (element->visuals.markArray)
        {
            DB64_ConvertOffsetRange((uintptr_t *)&element->visuals.markArray, bytes);
        }
    }
    else if (element->visualCount > 1)
    {
        const size_t bytes = element->visualCount * sizeof(FxElemVisuals);
        if ((uintptr_t)element->visuals.array == UINTPTR_MAX)
        {
            element->visuals.array = (FxElemVisuals *)DB_AllocStreamPos(15);
            Load_Stream(true, (uint8_t *)element->visuals.array, bytes);
            for (int i = 0; i < element->visualCount; ++i)
            {
                LoadVisual(&element->visuals.array[i], element->elemType);
            }
        }
        else if (element->visuals.array)
        {
            DB64_ConvertOffsetRange((uintptr_t *)&element->visuals.array, bytes);
        }
    }
    else
    {
        LoadVisual(&element->visuals.instance, element->elemType);
    }
    LoadEffectReference(&element->effectOnImpact);
    LoadEffectReference(&element->effectOnDeath);
    LoadEffectReference(&element->effectEmitted);
    if ((uintptr_t)element->trailDef == UINTPTR_MAX)
    {
        element->trailDef = (FxTrailDef *)DB_AllocStreamPos(15);
        DB64_LoadEffectTrail(element->trailDef, true);
    }
    else if (element->trailDef)
    {
        DB64_ConvertOffsetRange((uintptr_t *)&element->trailDef, sizeof(FxTrailDef));
    }
}

void DB64_LoadEffectAsset(XAssetHeader *header, bool atStreamStart)
{
    Load_Stream(atStreamStart, (uint8_t *)header, sizeof(XAssetHeader));
    if (DB64_LoadExternalAsset(ASSET_TYPE_FX, header))
    {
        return;
    }
    DB_PushStreamPos(0);
    const uintptr_t token = (uintptr_t)header->data;
    if (token == UINTPTR_MAX || token == UINTPTR_MAX - 1)
    {
        FxEffectDef *effect = (FxEffectDef *)DB_AllocStreamPos(15);
        header->fx = effect;
        const void **inserted = token == UINTPTR_MAX - 1 ? DB_InsertPointer() : NULL;
        Load_Stream(true, (uint8_t *)effect, sizeof(FxEffectDef));
        size_t count;
        if (!DB64_EffectElementCount(effect, &count))
        {
            Com_Error(ERR_DROP, "Invalid native effect element counts");
        }
        DB_PushStreamPos(4);
        DB64_LoadAssetString(&effect->name);
        if ((uintptr_t)effect->elemDefs == UINTPTR_MAX)
        {
            FxElemDef *elements = (FxElemDef *)DB_AllocStreamPos(15);
            effect->elemDefs = elements;
            Load_Stream(true, (uint8_t *)elements, count * sizeof(FxElemDef));
            for (size_t i = 0; i < count; ++i)
            {
                LoadElement(&elements[i]);
            }
        }
        else if (effect->elemDefs)
        {
            DB64_ConvertOffsetRange((uintptr_t *)&effect->elemDefs, count * sizeof(FxElemDef));
        }
        DB_PopStreamPos();
        Load_FxEffectDefAsset(header);
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

#include "db_external_assets.h"
#include <universal/q_shared.h>
#include <xanim/xmodel.h>
#include "database.h"
#include "db_model_assets.h"
#include "db_model_surfaces.h"
#include "db_material_assets.h"
#include "db_preset_assets.h"
#include "db_physics_geometry.h"
#include <limits.h>

static bool LoadArray(void **pointer, size_t size, int alignment)
{
    if ((uintptr_t)*pointer == UINTPTR_MAX)
    {
        *pointer = DB_AllocStreamPos(alignment);
        Load_Stream(true, (uint8_t *)*pointer, size);
        return true;
    }
    if (*pointer)
    {
        DB64_ConvertOffsetRange((uintptr_t *)pointer, size);
    }
    return false;
}

static void LoadModel(XModel *model)
{
    Load_Stream(true, (uint8_t *)model, sizeof(XModel));
    char error[256];
    if (!DB64_ValidateModelHeader(model, error, sizeof(error)))
    {
        Com_Error(ERR_DROP, "%s", error);
    }
    DB_PushStreamPos(4);
    DB64_LoadAssetString(&model->name);
    const unsigned int children = model->numBones - model->numRootBones;
    if (LoadArray((void **)&model->boneNames, model->numBones * sizeof(uint16_t), 1))
    {
        for (unsigned int i = 0; i < model->numBones; ++i)
        {
            Load_ScriptStringCustom(&model->boneNames[i]);
        }
    }
    LoadArray((void **)&model->parentList, children * sizeof(uint8_t), 0);
    for (unsigned int i = 0; i < children; ++i)
    {
        if (!model->parentList[i] || model->parentList[i] > i + model->numRootBones)
        {
            Com_Error(ERR_DROP, "Native model bone references an invalid parent");
        }
    }
    LoadArray((void **)&model->quats, 4 * children * sizeof(int16_t), 1);
    LoadArray((void **)&model->trans, 3 * children * sizeof(float), 15);
    LoadArray((void **)&model->partClassification, model->numBones * sizeof(uint8_t), 0);
    LoadArray((void **)&model->baseMat, model->numBones * sizeof(DObjAnimMat), 15);
    if (LoadArray((void **)&model->surfs, model->numsurfs * sizeof(XSurface), 15))
    {
        for (unsigned int i = 0; i < model->numsurfs; ++i)
        {
            DB64_LoadModelSurface(&model->surfs[i], false);
        }
    }
    for (unsigned int i = 0; i < model->numsurfs; ++i)
    {
        if (!DB64_ValidateModelSurface(&model->surfs[i], model->numBones, error, sizeof(error)))
        {
            Com_Error(ERR_DROP, "%s", error);
        }
    }
    if (LoadArray((void **)&model->materialHandles, model->numsurfs * sizeof(Material *), 15))
    {
        for (unsigned int i = 0; i < model->numsurfs; ++i)
        {
            if (!model->materialHandles[i])
            {
                Com_Error(ERR_DROP, "Native model has a null material");
            }
            DB64_LoadMaterialAsset((XAssetHeader *)&model->materialHandles[i], false);
        }
    }
    if (LoadArray((void **)&model->collSurfs, model->numCollSurfs * sizeof(XModelCollSurf_s), 15))
    {
        for (int i = 0; i < model->numCollSurfs; ++i)
        {
            XModelCollSurf_s *surface = &model->collSurfs[i];
            if (surface->numCollTris < 0 || surface->numCollTris > INT_MAX / sizeof(XModelCollTri_s) ||
                (!!surface->collTris != (surface->numCollTris != 0)) ||
                surface->boneIdx < (surface->contents ? 0 : -1) ||
                surface->boneIdx >= model->numBones)
            {
                Com_Error(ERR_DROP, "Invalid native model collision surface");
            }
            LoadArray((void **)&surface->collTris, surface->numCollTris * sizeof(XModelCollTri_s), 15);
        }
    }
    LoadArray((void **)&model->boneInfo, model->numBones * sizeof(XBoneInfo), 15);
    DB64_LoadPresetAsset(ASSET_TYPE_PHYSPRESET, (XAssetHeader *)&model->physPreset, false);
    DB64_LoadPhysicsGeometry(&model->physGeoms);
    DB_PopStreamPos();
}

void DB64_LoadModelAsset(XAssetHeader *header, bool atStreamStart)
{
    Load_Stream(atStreamStart, (uint8_t *)header, sizeof(XAssetHeader));
    if (DB64_LoadExternalAsset(ASSET_TYPE_XMODEL, header))
    {
        return;
    }
    DB_PushStreamPos(0);
    const uintptr_t token = (uintptr_t)header->data;
    if (token == UINTPTR_MAX || token == UINTPTR_MAX - 1)
    {
        header->model = (XModel *)DB_AllocStreamPos(15);
        const void **inserted = token == UINTPTR_MAX - 1 ? DB_InsertPointer() : NULL;
        LoadModel(header->model);
        Load_XModelAsset(header);
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

#include "db_external_assets.h"
#include <universal/q_shared.h>
#include "database.h"
#include "db_animation_assets.h"
#include <math.h>

static bool LoadArray(void **array, size_t size, int alignment)
{
    if ((uintptr_t)*array == UINTPTR_MAX)
    {
        *array = DB_AllocStreamPos(alignment);
        Load_Stream(true, (uint8_t *)*array, size);
        return true;
    }
    if (*array)
    {
        DB64_ConvertOffsetRange((uintptr_t *)array, size);
    }
    return false;
}

static void LoadIndices(XAnimDynamicIndices *indices, unsigned int size, unsigned int frames)
{
    const size_t bytes = (size + 1) * (frames < 256 ? sizeof(uint8_t) : sizeof(uint16_t));
    Load_Stream(true, (uint8_t *)indices, bytes);
    unsigned int previous = 0;
    for (unsigned int i = 0; i <= size; ++i)
    {
        const unsigned int index = frames < 256 ? indices->_1[i] : indices->_2[i];
        if (index > frames || (i && index <= previous))
        {
            Com_Error(ERR_DROP, "Invalid native animation delta frame index");
        }
        previous = index;
    }
}

static void LoadDelta(XAnimDeltaPart **delta, unsigned int frames)
{
    if (!LoadArray((void **)delta, sizeof(XAnimDeltaPart), 15))
    {
        return;
    }
    XAnimDeltaPart *value = *delta;
    if (value->trans)
    {
        if ((uintptr_t)value->trans != UINTPTR_MAX)
        {
            Com_Error(ERR_DROP, "Native animation delta translation must be inline");
        }
        value->trans = (XAnimPartTrans *)DB_AllocStreamPos(15);
        XAnimPartTrans *trans = value->trans;
        Load_Stream(true, (uint8_t *)trans, offsetof(XAnimPartTrans, u));
        if (trans->size > frames || trans->smallTrans > 1)
        {
            Com_Error(ERR_DROP, "Invalid native animation delta translation count");
        }
        if (trans->size)
        {
            Load_Stream(true, (uint8_t *)&trans->u, offsetof(XAnimPartTransFrames, indices));
            LoadIndices(&trans->u.frames.indices, trans->size, frames);
            if (!trans->u.frames.frames._1)
            {
                Com_Error(ERR_DROP, "Missing native animation delta translation frames");
            }
            LoadArray((void **)&trans->u.frames.frames._1,
                      (trans->size + 1) * 3 * (trans->smallTrans ? sizeof(uint8_t) : sizeof(uint16_t)),
                      trans->smallTrans ? 0 : 15);
        }
        else
        {
            Load_Stream(true, (uint8_t *)trans->u.frame0, sizeof(float[3]));
        }
    }
    if (value->quat)
    {
        if ((uintptr_t)value->quat != UINTPTR_MAX)
        {
            Com_Error(ERR_DROP, "Native animation delta rotation must be inline");
        }
        value->quat = (XAnimDeltaPartQuat *)DB_AllocStreamPos(15);
        XAnimDeltaPartQuat *quat = value->quat;
        Load_Stream(true, (uint8_t *)quat, offsetof(XAnimDeltaPartQuat, u));
        if (quat->size > frames)
        {
            Com_Error(ERR_DROP, "Invalid native animation delta rotation count");
        }
        if (quat->size)
        {
            Load_Stream(true, (uint8_t *)&quat->u, offsetof(XAnimDeltaPartQuatDataFrames, indices));
            LoadIndices(&quat->u.frames.indices, quat->size, frames);
            if (!quat->u.frames.frames)
            {
                Com_Error(ERR_DROP, "Missing native animation delta rotation frames");
            }
            LoadArray((void **)&quat->u.frames.frames, (quat->size + 1) * sizeof(int16_t[2]), 15);
        }
        else
        {
            Load_Stream(true, (uint8_t *)quat->u.frame0, sizeof(int16_t[2]));
        }
    }
}

static void LoadAnimation(XAnimParts *parts)
{
    Load_Stream(true, (uint8_t *)parts, sizeof(XAnimParts));
    char error[256];
    if (!DB64_ValidateAnimationHeader(parts, error, sizeof(error)))
    {
        Com_Error(ERR_DROP, "%s", error);
    }
    DB_PushStreamPos(4);
    DB64_LoadAssetString(&parts->name);
    if (LoadArray((void **)&parts->names, parts->boneCount[9] * sizeof(uint16_t), 1))
    {
        for (unsigned int i = 0; i < parts->boneCount[9]; ++i)
        {
            Load_ScriptStringCustom(&parts->names[i]);
        }
    }
    if (LoadArray((void **)&parts->notify, parts->notifyCount * sizeof(XAnimNotifyInfo), 15))
    {
        for (unsigned int i = 0; i < parts->notifyCount; ++i)
        {
            if (!isfinite(parts->notify[i].time) || parts->notify[i].time < 0 || parts->notify[i].time > 1)
            {
                Com_Error(ERR_DROP, "Invalid native animation notify time");
            }
            Load_ScriptStringCustom(&parts->notify[i].name);
        }
    }
    LoadDelta(&parts->deltaPart, parts->numframes);
    LoadArray((void **)&parts->dataByte, parts->dataByteCount * sizeof(uint8_t), 0);
    LoadArray((void **)&parts->dataShort, parts->dataShortCount * sizeof(int16_t), 1);
    LoadArray((void **)&parts->dataInt, parts->dataIntCount * sizeof(int), 15);
    LoadArray((void **)&parts->randomDataShort, parts->randomDataShortCount * sizeof(int16_t), 1);
    LoadArray((void **)&parts->randomDataByte, parts->randomDataByteCount * sizeof(uint8_t), 0);
    LoadArray((void **)&parts->randomDataInt, parts->randomDataIntCount * sizeof(int), 15);
    LoadArray(&parts->indices.data, parts->indexCount * (parts->numframes < 256 ? sizeof(uint8_t) : sizeof(uint16_t)),
              parts->numframes < 256 ? 0 : 1);
    DB_PopStreamPos();
}

void DB64_LoadAnimationAsset(XAssetHeader *header, bool atStreamStart)
{
    Load_Stream(atStreamStart, (uint8_t *)header, sizeof(XAssetHeader));
    if (DB64_LoadExternalAsset(ASSET_TYPE_XANIMPARTS, header))
    {
        return;
    }
    DB_PushStreamPos(0);
    const uintptr_t token = (uintptr_t)header->data;
    if (token == UINTPTR_MAX || token == UINTPTR_MAX - 1)
    {
        header->parts = (XAnimParts *)DB_AllocStreamPos(15);
        const void **inserted = token == UINTPTR_MAX - 1 ? DB_InsertPointer() : NULL;
        LoadAnimation(header->parts);
        Load_XAnimPartsAsset(header);
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

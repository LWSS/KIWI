#include <universal/q_shared.h>
#include "xanim.h"
#include "xmodel.h"
#include <gfx_d3d/r_model.h>
#include <universal/com_files.h>
#include <qcommon/qcommon.h>

enum $69AF8E44C9D6025F282D494F15F1F016 : int
{
    PART_TYPE_NO_QUAT = 0x0,
    PART_TYPE_HALF_QUAT = 0x1,
    PART_TYPE_FULL_QUAT = 0x2,
    PART_TYPE_HALF_QUAT_NO_SIZE = 0x3,
    PART_TYPE_FULL_QUAT_NO_SIZE = 0x4,
    PART_TYPE_SMALL_TRANS = 0x5,
    PART_TYPE_TRANS = 0x6,
    PART_TYPE_TRANS_NO_SIZE = 0x7,
    PART_TYPE_NO_TRANS = 0x8,
    PART_TYPE_ALL = 0x9,
    PART_TYPE_COUNT = 0xA,
};

const char *g_highPrecisionParts[18] =
{
  "j_ankle_le",
  "j_ankle_ri",
  "j_clavicle_le",
  "j_clavicle_ri",
  "j_elbow_le",
  "j_elbow_ri",
  "j_hip_le",
  "j_hip_ri",
  "j_knee_le",
  "j_knee_ri",
  "j_mainroot",
  "j_shoulder_le",
  "j_shoulder_ri",
  "j_spine4",
  "j_spinelower",
  "j_spineupper",
  "j_wrist_le",
  "j_wrist_ri"
}; // idb

int g_highPrecisionPart[128];

HunkUser *g_animUser;
XAnimPartQuatPtr *g_partQuatArray;
XAnimPartTransPtr *g_partTransArray;
char *g_simpleQuatBits;

uint __cdecl XAnimGetPartQuatType(uint animPartIndex)
{
    XAnimPartQuat *quat; // [esp+0h] [ebp-4h]

    quat = g_partQuatArray[animPartIndex].quat;
    if (((1 << (animPartIndex & 7)) & g_simpleQuatBits[animPartIndex >> 3]) != 0)
    {
        if (quat)
        {
            if (quat->size)
                return 1;
            else
                return 3;
        }
        else
        {
            return 0;
        }
    }
    else
    {
        iassert(quat);
        if (quat->size)
            return 2;
        else
            return 4;
    }
}

uint __cdecl XAnimGetPartTransType(uint animPartIndex)
{
    XAnimPartTrans *trans; // [esp+0h] [ebp-4h]

    trans = g_partTransArray[animPartIndex].trans;
    if (!trans)
        return PART_TYPE_NO_TRANS;
    if (!trans->size)
        return PART_TYPE_TRANS_NO_SIZE;
    if (trans->smallTrans)
        return PART_TYPE_SMALL_TRANS;
    return PART_TYPE_TRANS;
}

int __cdecl XAnimCompareTransParts(const void* arg1, const void* arg2)
{
    uint16_t *p0 = (uint16_t *)arg1;
    uint16_t *p1 = (uint16_t *)arg2;
    uint PartTransType; // eax
    uint partArrayIndices[2]; // [esp+0h] [ebp-18h]
    int diff; // [esp+8h] [ebp-10h]
    uint type[2]; // [esp+Ch] [ebp-Ch]
    uint i; // [esp+14h] [ebp-4h]

    partArrayIndices[0] = *p0;
    partArrayIndices[1] = *p1;
    for (i = 0; i < 2; ++i)
    {
        PartTransType = XAnimGetPartTransType(partArrayIndices[i]);
        type[i] = PartTransType;
    }
    diff = type[0] - type[1];
    if (type[0] == type[1])
        return g_partTransArray[partArrayIndices[0]].partIndex - g_partTransArray[partArrayIndices[1]].partIndex;
    else
        return diff;
}

void __cdecl XAnimEmitFrameIndices(
    uint tableSize,
    unsigned __int8 **dataByte,
    __int16 **dataShort,
    XAnimIndices *indices,
    XAnimDynamicIndices *frameIndices,
    bool useSmallIndices)
{
    uint i; // [esp+4h] [ebp-8h]
    uint longTableSize; // [esp+8h] [ebp-4h]

    if (useSmallIndices)
    {
        memcpy(*dataByte, frameIndices->_1, tableSize + 1);
        *dataByte += tableSize + 1;
    }
    else if (tableSize >= 0x40)
    {
        memcpy(indices->_1, frameIndices->_1, 2 * tableSize + 2);
        indices->_1 += 2 * tableSize + 2;
        longTableSize = ((tableSize - 1) >> 8) + 1;
        iassert(!frameIndices->_2[0]);
        for (i = 0; i < longTableSize; ++i)
            (*dataShort)[i] = frameIndices[256 * i]._2[0];
        (*dataShort)[longTableSize] = frameIndices[tableSize]._2[0];
        *dataShort += longTableSize + 1;
    }
    else
    {
        memcpy(*dataShort, frameIndices->_1, 2 * tableSize + 2);
        *dataShort += tableSize + 1;
    }
}

int __cdecl XAnimCompareQuatParts(const void *arg1, const void *arg2)
{
    uint16_t *p0 = (uint16_t*)arg1;
    uint16_t *p1 = (uint16_t *)arg2;

    uint PartQuatType; // eax
    uint partArrayIndices[2]; // [esp+0h] [ebp-18h]
    int diff; // [esp+8h] [ebp-10h]
    uint type[2]; // [esp+Ch] [ebp-Ch]
    uint i; // [esp+14h] [ebp-4h]

    partArrayIndices[0] = *p0;
    partArrayIndices[1] = *p1;
    for (i = 0; i < 2; ++i)
    {
        PartQuatType = XAnimGetPartQuatType(partArrayIndices[i]);
        type[i] = PartQuatType;
    }
    diff = type[0] - type[1];
    if (type[0] == type[1])
        return g_partQuatArray[partArrayIndices[0]].partIndex - g_partQuatArray[partArrayIndices[1]].partIndex;
    else
        return diff;
}

int __cdecl XAnimIsHighPrecisionPart(const char *name)
{
    int v2; // [esp+4h] [ebp-20h]
    uint low; // [esp+18h] [ebp-Ch]
    uint high; // [esp+1Ch] [ebp-8h]

    low = 0;
    high = 18;
    while (low < high)
    {
        v2 = strcmp(name, g_highPrecisionParts[(high + low) >> 1]);
        if (v2 >= 0)
        {
            if (v2 <= 0)
                return 1;
            low = ((high + low) >> 1) + 1;
        }
        else
        {
            high = (high + low) >> 1;
        }
    }
    return 0;
}

void* XAnimTempAlloc(int size)
{
    iassert(g_animUser);

    return Hunk_UserAlloc(g_animUser, size, alignof(void *));
}

XModelPieces *__cdecl XModelPiecesLoadFile(const char *name, void *(__cdecl *Alloc)(int))
{
    XModelPiece *piece;          // [esp+38h] [ebp-A4h]
    int pieceIndex;              // [esp+3Ch] [ebp-A0h]
    unsigned __int8 *pos;        // [esp+40h] [ebp-9Ch]
    char filename[64];           // [esp+44h] [ebp-98h] BYREF
    char piecename[64];          // [esp+84h] [ebp-58h] BYREF
    unsigned __int8 *buf = NULL; // [esp+C8h] [ebp-14h] BYREF
    int version;                 // [esp+CCh] [ebp-10h]
    XModelPieces *xmodelPieces;  // [esp+D0h] [ebp-Ch]
    int len;                     // [esp+D4h] [ebp-8h]
    int fileSize;                // [esp+D8h] [ebp-4h]

    if (Com_sprintf(filename, 0x40u, "xmodelpieces/%s", name) < 0)
    {
        Com_PrintError(CON_CHANNEL_ANIM, "ERROR: filename '%s' too long\n", filename);
        return 0;
    }

    fileSize = FS_ReadFile(filename, (void **)&buf);

    if (fileSize < 0)
    {
        iassert(!buf);
        Com_PrintError(CON_CHANNEL_ANIM, "ERROR: xmodelpieces '%s' not found\n", name);
        return 0;
    }

    if (fileSize < 4)
    {
        Com_PrintError(CON_CHANNEL_ANIM, "ERROR: xmodelpieces '%s' has an incomplete header\n", name);
        FS_FreeFile((char *)buf);
        return 0;
    }

    iassert(buf);

    pos = buf;
    version = Buf_Read<ushort>(&pos);

    if (version == 1)
    {
        xmodelPieces = (XModelPieces *)Alloc(sizeof(XModelPieces));
        iassert(xmodelPieces);
        xmodelPieces->numpieces = Buf_Read<ushort>(&pos);
        xmodelPieces->pieces = (XModelPiece *)Alloc(sizeof(XModelPiece) * xmodelPieces->numpieces);
        for (pieceIndex = 0; pieceIndex < xmodelPieces->numpieces; ++pieceIndex)
        {
            piece = &xmodelPieces->pieces[pieceIndex];
            len = strlen((const char *)pos) + 1;
            if (len > 64)
            {
                Com_PrintError(CON_CHANNEL_ANIM, "ERROR: piecename '%s' too long\n", (const char *)pos);
                FS_FreeFile((char *)buf);
                return 0;
            }

            char *v5 = (char *)pos;
            char *v4 = piecename;
            char v3;
            do
            {
                v3 = *v5;
                *v4++ = *v5++;
            } while (v3);
            pos = &pos[len];

            piece->model = R_RegisterModel(piecename);
            if (!piece->model)
            {
                Com_PrintError(CON_CHANNEL_ERROR, "ERROR: xmodel piece '%s' missing from pieces model 's%'\n", piecename, filename);
                FS_FreeFile((char *)buf);
                return 0;
            }
            piece->offset[0] = Buf_Read<float>(&pos);
            piece->offset[1] = Buf_Read<float>(&pos);
            piece->offset[2] = Buf_Read<float>(&pos);
        }
        FS_FreeFile((char *)buf);
        return xmodelPieces;
    }
    else
    {
        FS_FreeFile((char *)buf);
        Com_PrintError(CON_CHANNEL_ANIM, "ERROR: xmodelpieces '%s' out of date (version %d, expecting %d).\n", name, version, 1);
        return 0;
    }
}

XModelPieces *__cdecl XModelPiecesPrecache(const char *name, void *(__cdecl *Alloc)(int))
{
    XModelPieces *xmodelPieces; // [esp+0h] [ebp-4h]

    iassert(name);
    iassert(name[0]);

    xmodelPieces = (XModelPieces *)Hunk_FindDataForFile(8, name);

    if (xmodelPieces)
        return xmodelPieces;

    xmodelPieces = XModelPiecesLoadFile(name, Alloc);
    if (xmodelPieces)
    {
        xmodelPieces->name = Hunk_SetDataForFile(8, name, xmodelPieces, Alloc);
        return xmodelPieces;
    }
    else
    {
        Com_PrintError(CON_CHANNEL_PHYS, "ERROR: Cannot find xmodel pieces '%s'.\n", name);
        return 0;
    }
}

unsigned __int8 *__cdecl LoadTrans(
    void *(__cdecl *Alloc)(int),
    unsigned __int8 *pos,
    uint16_t numTransIndices,
    XAnimPartTrans *trans)
{
    int size; // [esp+20h] [ebp-4h]

    trans->smallTrans = Buf_Read<byte>(&pos);

    trans->u.frames.mins[0] = Buf_Read<float>(&pos);
    trans->u.frames.mins[1] = Buf_Read<float>(&pos);
    trans->u.frames.mins[2] = Buf_Read<float>(&pos);

    trans->u.frames.size[0] = Buf_Read<float>(&pos);
    trans->u.frames.size[1] = Buf_Read<float>(&pos);
    trans->u.frames.size[2] = Buf_Read<float>(&pos);

    if (trans->smallTrans)
        Vec3Scale(trans->u.frames.size, 0.0039215689f, trans->u.frames.size);
    else
        Vec3Scale(trans->u.frames.size, 0.000015259022f, trans->u.frames.size);

    trans->size = numTransIndices - 1;

    if (trans->smallTrans)
        size = 3 * numTransIndices;
    else
        size = 6 * numTransIndices;

    trans->u.frames.frames._1 = (unsigned __int8 (*)[3])Alloc(size);

    memcpy(trans->u.frames.frames._1, pos, size);
    return &pos[size];
}

void __cdecl ConsumeQuat(unsigned __int8 **pos, __int16 *out)
{
    int v2; // [esp+8h] [ebp-34h]
    float v3; // [esp+Ch] [ebp-30h]
    float v4; // [esp+10h] [ebp-2Ch]
    float v5; // [esp+14h] [ebp-28h]

    int temp; // [esp+38h] [ebp-4h]

    out[0] = Buf_Read<short>(pos);
    out[1] = Buf_Read<short>(pos);
    out[2] = Buf_Read<short>(pos);

    temp = 1073676289 - (out[2] * out[2] + out[1] * out[1] + *out * *out);

    if (temp <= 0)
    {
        v2 = 0;
    }
    else
    {
        v5 = sqrt(temp);
        v4 = v5 + 0.5;
        v3 = floor(v4);
        v2 = v3;
    }

    if (v2 != v2)
        MyAssertHandler(".\\xanim\\xanim_load_obj.cpp", 112, 0, "%s", "iQ[3] == (short)iQ[3]");

    out[3] = v2;
}

void __cdecl ConsumeQuat2(unsigned __int8 **pos, __int16 *out)
{
    int v2; // [esp+8h] [ebp-24h]
    float v3; // [esp+Ch] [ebp-20h]
    float v4; // [esp+10h] [ebp-1Ch]
    float v5; // [esp+14h] [ebp-18h]
    int temp; // [esp+28h] [ebp-4h]

    out[0] = Buf_Read<ushort>(pos);

    temp = 1073676289 - out[0] * out[0];
    if (temp <= 0)
    {
        v2 = 0;
    }
    else
    {
        v5 = sqrt(temp);
        v4 = v5 + 0.5;
        v3 = floor(v4);
        v2 = v3;
    }

    if (v2 != v2)
        MyAssertHandler(".\\xanim\\xanim_load_obj.cpp", 130, 0, "%s", "iQ[1] == (short)iQ[1]");

    out[1] = v2;
}

void __cdecl ConsumeQuatNoSwap(unsigned __int8 **pos, __int16 *out)
{
    int v2; // [esp+8h] [ebp-34h]
    float v3; // [esp+Ch] [ebp-30h]
    float v4; // [esp+10h] [ebp-2Ch]
    float v5; // [esp+14h] [ebp-28h]
    int temp; // [esp+38h] [ebp-4h]

    out[0] = Buf_Read<ushort>(pos);
    out[1] = Buf_Read<ushort>(pos);
    out[2] = Buf_Read<ushort>(pos);

    temp = 1073676289 - (out[2] * out[2] + out[1] * out[1] + out[0] * out[0]);

    if (temp <= 0)
    {
        v2 = 0;
    }
    else
    {
        v5 = sqrt(temp);
        v4 = v5 + 0.5;
        v3 = floor(v4);
        v2 = v3;
    }

    if (v2 != v2)
        MyAssertHandler(".\\xanim\\xmodel_load_obj.cpp", 60, 0, "%s", "iQ[3] == (short)iQ[3]");

    out[3] = v2;
}

unsigned __int8 *__cdecl GetDeltaQuaternions(
    XAnimDeltaPart *deltaPart,
    void *(__cdecl *Alloc)(int),
    unsigned __int8 *pos,
    uint16_t numloopframes,
    bool useSmallIndices)
{
    __int16 *sQ;             // [esp+4h] [ebp-30h]
    int m;                   // [esp+10h] [ebp-24h]
    int k;                   // [esp+14h] [ebp-20h]
    int j;                   // [esp+18h] [ebp-1Ch]
    int i;                   // [esp+1Ch] [ebp-18h]
    __int16 quat[4];         // [esp+20h] [ebp-14h] BYREF
    int size;                // [esp+2Ch] [ebp-8h]
    uint16_t numQuatIndices; // [esp+30h] [ebp-4h]

    numQuatIndices = Buf_Read<ushort>(&pos);

    if (numQuatIndices)
    {
        if (numQuatIndices == 1)
        {
            ConsumeQuat2(&pos, quat);
            deltaPart->quat = (XAnimDeltaPartQuat *)Alloc(sizeof(XAnimDeltaPartQuat));
            deltaPart->quat->size = 0;
            deltaPart->quat->u.frame0[0] = quat[0];
            deltaPart->quat->u.frame0[1] = quat[1];
        }
        else
        {
            iassert(numQuatIndices <= numloopframes);

            if (useSmallIndices)
            {
                deltaPart->quat = (XAnimDeltaPartQuat *)Alloc(sizeof(XAnimDeltaPartQuat) + sizeof(byte) * numQuatIndices);
                if (numQuatIndices >= numloopframes)
                {
                    for (i = 0; i < numQuatIndices; ++i)
                    {
                        deltaPart->quat->u.frames.indices._1[i] = i;
                    }
                }
                else
                {
                    size = numQuatIndices;
                    memcpy(deltaPart->quat->u.frames.indices._1, pos, numQuatIndices);
                    pos += numQuatIndices;
                }
            }
            else
            {
                deltaPart->quat = (XAnimDeltaPartQuat *)Alloc(sizeof(XAnimDeltaPartQuat) + sizeof(uint16_t) * numQuatIndices);
                if (numQuatIndices >= numloopframes)
                {
                    for (j = 0; j < numQuatIndices; ++j)
                    {
                        deltaPart->quat->u.frames.indices._2[j] = j;
                    }
                }
                else
                {
                    size = 2 * numQuatIndices;
                    memcpy(deltaPart->quat->u.frames.indices._1, pos, size);
                    pos += size;
                }
            }
            deltaPart->quat->size = numQuatIndices - 1;
            deltaPart->quat->u.frames.frames = (__int16(*)[2])Alloc(4 * numQuatIndices);
            for (k = 0; k < numQuatIndices; ++k)
            {
                ConsumeQuat2(&pos, quat);
                deltaPart->quat->u.frames.frames[k][0] = quat[0];
                deltaPart->quat->u.frames.frames[k][1] = quat[1];
            }
            for (m = 1; m < numQuatIndices; ++m)
            {
                sQ = deltaPart->quat->u.frames.frames[m];
                if (*(sQ - 1) * sQ[1] + *(sQ - 2) * *sQ < 0)
                {
                    *sQ = -*sQ;
                    sQ[1] = -sQ[1];
                }
            }
        }
    }
    else
    {
        deltaPart->quat = 0;
    }
    return pos;
}

unsigned __int8 *__cdecl GetDeltaTranslations(
    XAnimDeltaPart *deltaPart,
    void *(__cdecl *Alloc)(int),
    unsigned __int8 *pos,
    uint16_t numloopframes,
    bool useSmallIndices)
{
    XAnimPartTransData *p_u;  // edx
    uint16_t numTransIndices; // [esp+10h] [ebp-20h]
    int j;                    // [esp+14h] [ebp-1Ch]
    int i;                    // [esp+18h] [ebp-18h]

    float mins[3];

    numTransIndices = Buf_Read<ushort>(&pos);

    if (numTransIndices)
    {
        if (numTransIndices == 1)
        {
            mins[0] = Buf_Read<float>(&pos);
            mins[1] = Buf_Read<float>(&pos);
            mins[2] = Buf_Read<float>(&pos);

            deltaPart->trans = (XAnimPartTrans *)Alloc(sizeof(XAnimPartTrans));
            deltaPart->trans->size = 0;
            p_u = &deltaPart->trans->u;
            p_u->frames.mins[0] = mins[0];
            p_u->frames.mins[1] = mins[1];
            p_u->frames.mins[2] = mins[2];
        }
        else
        {
            iassert(numTransIndices <= numloopframes);

            if (useSmallIndices)
            {
                deltaPart->trans = (XAnimPartTrans *)Alloc(sizeof(XAnimPartTrans) + sizeof(byte) * numTransIndices);
                if (numTransIndices >= numloopframes)
                {
                    for (i = 0; i < numTransIndices; ++i)
                    {
                        deltaPart->trans->u.frames.indices._1[i] = i;
                    }
                }
                else
                {
                    memcpy(deltaPart->trans->u.frames.indices._1, pos, numTransIndices);
                    pos += numTransIndices;
                }
            }
            else
            {
                deltaPart->trans = (XAnimPartTrans *)Alloc(sizeof(XAnimPartTrans) + sizeof(uint16_t) * numTransIndices);
                if (numTransIndices >= numloopframes)
                {
                    for (j = 0; j < numTransIndices; ++j)
                    {
                        deltaPart->trans->u.frames.indices._2[j] = j;
                    }
                }
                else
                {
                    memcpy(deltaPart->trans->u.frames.indices._1, pos, 2 * numTransIndices);
                    pos += 2 * numTransIndices;
                }
            }
            return LoadTrans(Alloc, pos, numTransIndices, deltaPart->trans);
        }
    }
    else
    {
        deltaPart->trans = 0;
    }
    return pos;
}

unsigned __int8 *__cdecl GetQuaternions(
    XAnimPartQuatPtr *part,
    unsigned __int8 *pos,
    bool bFlipQuat,
    bool bSimpleQuat,
    uint16_t numloopframes,
    bool useSmallIndices)
{
    __int16 *v8;             // [esp+4h] [ebp-44h]
    __int16 *v9;             // [esp+8h] [ebp-40h]
    int ii;                  // [esp+10h] [ebp-38h]
    int n;                   // [esp+14h] [ebp-34h]
    __int16 *sQ;             // [esp+18h] [ebp-30h]
    int m;                   // [esp+24h] [ebp-24h]
    int k;                   // [esp+28h] [ebp-20h]
    int j;                   // [esp+2Ch] [ebp-1Ch]
    int i;                   // [esp+30h] [ebp-18h]
    __int16 quat[4];         // [esp+34h] [ebp-14h] BYREF
    int size;                // [esp+40h] [ebp-8h]
    uint16_t numQuatIndices; // [esp+44h] [ebp-4h]

    numQuatIndices = Buf_Read<ushort>(&pos);

    if (numQuatIndices)
    {
        if (numQuatIndices == 1)
        {
            if (bSimpleQuat)
            {
                ConsumeQuat2(&pos, quat);
                if (bFlipQuat)
                {
                    quat[0] = -quat[0];
                    quat[1] = -quat[1];
                }
                part->quat = (XAnimPartQuat *)XAnimTempAlloc(sizeof(XAnimPartQuat));
                part->quat->u.frame0[0] = quat[0];
                part->quat->u.frame0[1] = quat[1];
            }
            else
            {
                ConsumeQuat(&pos, quat);
                if (bFlipQuat)
                {
                    quat[0] = -quat[0];
                    quat[1] = -quat[1];
                    quat[2] = -quat[2];
                    quat[3] = -quat[3];
                }
                part->quat = (XAnimPartQuat *)XAnimTempAlloc(sizeof(XAnimPartQuat));
                // part->quat->u = *(XAnimPartQuatData*)quat;
                part->quat->u.frame0[0] = quat[0];
                part->quat->u.frame0[1] = quat[1];
                part->quat->u.frame0[2] = quat[2];
                part->quat->u.frame0[3] = quat[3];
            }
            part->quat->size = 0;
        }
        else
        {
            iassert(numQuatIndices <= numloopframes);

            if (useSmallIndices)
            {
                part->quat = (XAnimPartQuat *)XAnimTempAlloc(sizeof(XAnimPartQuat) + sizeof(byte) * numQuatIndices);
                if (numQuatIndices >= numloopframes)
                {
                    for (i = 0; i < numQuatIndices; ++i)
                    {
                        part->quat->u.frames.indices._1[i] = i;
                    }
                }
                else
                {
                    size = numQuatIndices;
                    memcpy(part->quat->u.frames.indices._1, pos, numQuatIndices);
                    pos += numQuatIndices;
                }
            }
            else
            {
                part->quat = (XAnimPartQuat *)XAnimTempAlloc(sizeof(XAnimPartQuat) + sizeof(uint16_t) * numQuatIndices);
                if (numQuatIndices >= numloopframes)
                {
                    for (j = 0; j < numQuatIndices; ++j)
                    {
                        part->quat->u.frames.indices._2[j] = j;
                    }
                }
                else
                {
                    size = 2 * numQuatIndices;
                    memcpy(part->quat->u.frames.indices._1, pos, size);
                    pos += size;
                }
            }
            if (bSimpleQuat)
            {
                part->quat->u.frames.u.frames = (__int16(*)[4])XAnimTempAlloc(4 * numQuatIndices);
                ConsumeQuat2(&pos, quat);
                if (bFlipQuat)
                {
                    quat[0] = -quat[0];
                    quat[1] = -quat[1];
                }
                //*(_WORD *)part->quat->u.frames.u.frames = quat[0];
                (*part->quat->u.frames.u.frames)[0] = quat[0];
                (*part->quat->u.frames.u.frames)[1] = quat[1];
                for (k = 1; k < numQuatIndices; ++k)
                {
                    ConsumeQuat2(&pos, quat);
                    (*part->quat->u.frames.u.frames)[2 * k] = quat[0];
                    (*part->quat->u.frames.u.frames)[2 * k + 1] = quat[1];
                }
                for (m = 1; m < numQuatIndices; ++m)
                {
                    sQ = &(*part->quat->u.frames.u.frames)[2 * m];
                    if (*(sQ - 1) * sQ[1] + *(sQ - 2) * *sQ < 0)
                    {
                        *sQ = -*sQ;
                        sQ[1] = -sQ[1];
                    }
                }
            }
            else
            {
                part->quat->u.frames.u.frames = (__int16(*)[4])XAnimTempAlloc(8 * numQuatIndices);
                ConsumeQuat(&pos, quat);
                if (bFlipQuat)
                {
                    quat[0] = -quat[0];
                    quat[1] = -quat[1];
                    quat[2] = -quat[2];
                    quat[3] = -quat[3];
                }
                memcpy(part->quat->u.frames.u.frames[0], quat, sizeof(int16_t[4]));
                for (n = 1; n < numQuatIndices; ++n)
                {
                    ConsumeQuat(&pos, quat);
                    part->quat->u.frames.u.frames[n][0] = quat[0];
                    part->quat->u.frames.u.frames[n][1] = quat[1];
                    part->quat->u.frames.u.frames[n][2] = quat[2];
                    part->quat->u.frames.u.frames[n][3] = quat[3];
                }
                for (ii = 1; ii < numQuatIndices; ++ii)
                {
                    v8 = part->quat->u.frames.u.frames[ii];
                    v9 = v8 - 4;
                    if (v9[3] * v8[3] + v9[2] * v8[2] + v9[1] * v8[1] + *v9 * *v8 < 0)
                    {
                        v8[0] = -v8[0];
                        v8[1] = -v8[1];
                        v8[2] = -v8[2];
                        v8[3] = -v8[3];
                    }
                }
            }
            part->quat->size = numQuatIndices - 1;
        }
    }
    else
    {
        iassert(bSimpleQuat);
        part->quat = 0;
    }
    return pos;
}

unsigned __int8 *__cdecl GetTranslations(
    XAnimPartTransPtr *part,
    unsigned __int8 *pos,
    uint16_t numloopframes,
    bool useSmallIndices)
{
    XAnimPartTransData *p_u;  // edx
    uint16_t numTransIndices; // [esp+10h] [ebp-20h]
    int j;                    // [esp+14h] [ebp-1Ch]
    int i;                    // [esp+18h] [ebp-18h]

    float mins[3];

    numTransIndices = Buf_Read<ushort>(&pos);

    if (numTransIndices)
    {
        if (numTransIndices == 1)
        {
            mins[0] = Buf_Read<float>(&pos);
            mins[1] = Buf_Read<float>(&pos);
            mins[2] = Buf_Read<float>(&pos);

            part->trans = (XAnimPartTrans *)XAnimTempAlloc(sizeof(XAnimPartTrans));
            part->trans->size = 0;
            p_u = &part->trans->u;
            p_u->frames.mins[0] = mins[0];
            p_u->frames.mins[1] = mins[1];
            p_u->frames.mins[2] = mins[2];
        }
        else
        {
            iassert(numTransIndices <= numloopframes);

            if (useSmallIndices)
            {
                part->trans = (XAnimPartTrans *)XAnimTempAlloc(sizeof(XAnimPartTrans) + sizeof(byte) * numTransIndices);
                if (numTransIndices >= numloopframes)
                {
                    for (i = 0; i < numTransIndices; ++i)
                    {
                        part->trans->u.frames.indices._1[i] = i;
                    }
                }
                else
                {
                    memcpy(part->trans->u.frames.indices._1, pos, numTransIndices);
                    pos += numTransIndices;
                }
            }
            else
            {
                part->trans = (XAnimPartTrans *)XAnimTempAlloc(sizeof(XAnimPartTrans) + sizeof(uint16_t) * numTransIndices);
                if (numTransIndices >= numloopframes)
                {
                    for (j = 0; j < numTransIndices; ++j)
                    {
                        part->trans->u.frames.indices._2[j] = j;
                    }
                }
                else
                {
                    memcpy(part->trans->u.frames.indices._1, pos, 2 * numTransIndices);
                    pos += 2 * numTransIndices;
                }
            }
            return LoadTrans(XAnimTempAlloc, pos, numTransIndices, part->trans);
        }
    }
    else
    {
        part->trans = 0;
    }
    return pos;
}

void __cdecl ReadNoteTracks(const char *name, byte **pos, XAnimParts *parts, void *(__cdecl *Alloc)(int))
{
    double v5;               // [esp+8h] [ebp-30h]
    uint16_t v6;             // [esp+24h] [ebp-14h]
    int numNoteTracks;       // [esp+28h] [ebp-10h]
    XAnimNotifyInfo *notify; // [esp+30h] [ebp-8h]
    int i;                   // [esp+34h] [ebp-4h]

    numNoteTracks = Buf_Read<byte>(pos);

    if (numNoteTracks >= 255)
    {
        Com_Error(ERR_DROP, "ReadNoteTracks: too many notifications");
    }

    parts->notifyCount = numNoteTracks + 1;
    notify = (XAnimNotifyInfo *)Alloc(sizeof(XAnimNotifyInfo) * parts->notifyCount);
    parts->notify = notify;
    for (i = 0; i < numNoteTracks; ++i)
    {
        notify->name = SL_GetString_((const char *)*pos, 0, MT_TYPE_NOTETRACK);
        *pos += strlen((const char *)*pos) + 1;

        v6 = Buf_Read<ushort>(pos);

        if (parts->numframes)
        {
            v5 = (double)v6 / parts->numframes;
        }
        else
        {
            v5 = 0.0;
        }
        notify->time = v5;

        iassert(notify->time >= 0.0f && notify->time <= 1.0f);
        ++notify;
    }
    notify->name = SL_GetString_("end", 0, MT_TYPE_NOTETRACK);
    notify->time = 1.0;
}

int g_checkBoneOrder;

XAnimParts *__cdecl XAnimLoadFile(char *name, void *(__cdecl *Alloc)(int))
{
    __int16 prev;                 // ax
    unsigned __int8 *Quaternions; // eax
    void *v12;                    // [esp+8h] [ebp-14DCh]
    void *v13;                    // [esp+Ch] [ebp-14D8h]
    int *v14;                     // [esp+10h] [ebp-14D4h]
    unsigned __int8 *v15;         // [esp+14h] [ebp-14D0h]
    unsigned __int8 *v16;         // [esp+18h] [ebp-14CCh]
    int *v17;                     // [esp+1Ch] [ebp-14C8h]
    __int16 *v18;                 // [esp+20h] [ebp-14C4h]
    unsigned __int8 *v19;         // [esp+24h] [ebp-14C0h]
    int v20;                      // [esp+28h] [ebp-14BCh]
    int v21;                      // [esp+2Ch] [ebp-14B8h]
    int v22;                      // [esp+30h] [ebp-14B4h]
    int v23;                      // [esp+34h] [ebp-14B0h]
    int v24;                      // [esp+38h] [ebp-14ACh]
    int v25;                      // [esp+3Ch] [ebp-14A8h]
    int v26;                      // [esp+40h] [ebp-14A4h]
    BOOL v27;                     // [esp+44h] [ebp-14A0h]
    uint16_t totalframes;         // [esp+66h] [ebp-147Eh]
    int v30;                      // [esp+B0h] [ebp-1434h]
    int v31;                      // [esp+B4h] [ebp-1430h]
    int v32;                      // [esp+B8h] [ebp-142Ch]
    int v33;                      // [esp+BCh] [ebp-1428h]
    unsigned __int8 v34;          // [esp+C0h] [ebp-1424h]
    __int16 numBones;             // [esp+C8h] [ebp-141Ch]
    __int16 version;              // [esp+D0h] [ebp-1414h]
    unsigned __int8 *pos;         // [esp+DCh] [ebp-1408h] BYREF
    bool useSmallIndices;         // [esp+E3h] [ebp-1401h]
    unsigned __int8 dst[16];      // [esp+E4h] [ebp-1400h] BYREF
    uint i;                       // [esp+F4h] [ebp-13F0h]
    char dest[64];
    XAnimPartTransPtr sortedTrans[128]; // [esp+FCh] [ebp-13E8h] BYREF
    __int16 *dataShort;                 // [esp+540h] [ebp-FA4h] BYREF
    uint animPartIndex;                 // [esp+544h] [ebp-FA0h]
    uint dataByteCount;                 // [esp+548h] [ebp-F9Ch]
    unsigned __int8 *dataByte;          // [esp+54Ch] [ebp-F98h] BYREF
    int *randomDataInt;                 // [esp+550h] [ebp-F94h]
    int dataIntCount;                   // [esp+554h] [ebp-F90h]
    int v51[10];                        // [esp+558h] [ebp-F8Ch] BYREF
    uint16_t numframes;                 // [esp+584h] [ebp-F60h]
    unsigned __int8 *buf;               // [esp+588h] [ebp-F5Ch] BYREF
    uint count;                         // [esp+58Ch] [ebp-F58h]
    uint v56;                           // [esp+590h] [ebp-F54h]
    int *dataInt;                       // [esp+594h] [ebp-F50h]
    int fileSize;                       // [esp+598h] [ebp-F4Ch]
    _WORD base[130];                    // [esp+59Ch] [ebp-F48h] BYREF
    uint randomDataShortCount;          // [esp+6A0h] [ebp-E44h]
    unsigned __int8 *randomDataShort;   // [esp+6A4h] [ebp-E40h]
    int indexCount;                     // [esp+6A8h] [ebp-E3Ch]
    unsigned __int8 *v63;               // [esp+6ACh] [ebp-E38h]
    unsigned __int8 *randomDataByte;    // [esp+6B0h] [ebp-E34h]
    uint tableSize;                     // [esp+6B4h] [ebp-E30h]
    XAnimIndices indices;               // [esp+6B8h] [ebp-E2Ch] BYREF
    XAnimPartQuatPtr part[128];         // [esp+6BCh] [ebp-E28h] BYREF
    _WORD v68[128];                     // [esp+ABCh] [ebp-A28h]
    BOOL v69;                           // [esp+BBCh] [ebp-928h]
    uint PartQuatType;                  // [esp+BC0h] [ebp-924h]
    XAnimPartQuatPtr sortedQuats[128];  // [esp+BC4h] [ebp-920h]
    ushort boneIndexes[130];            // [esp+FC4h] [ebp-520h] BYREF
    XAnimPartTransPtr v73[128];         // [esp+10CCh] [ebp-418h] BYREF
    XAnimParts *parts;                  // [esp+14CCh] [ebp-18h]
    uint16_t numLoopFrames;             // [esp+14D0h] [ebp-14h]
    char partFlags;                     // [esp+14D7h] [ebp-Dh]
    int dataShortCount;                 // [esp+14D8h] [ebp-Ch]
    int randomDataByteCount;            // [esp+14DCh] [ebp-8h]
    int randomDataIntCount;             // [esp+14E0h] [ebp-4h]

    iassert(sizeof(XAnimPartTransPtr) == (sizeof(void *) == 8 ? 16 : 8));
    iassert(sizeof(XAnimParts) == (sizeof(void *) == 8 ? 136 : 88));

#ifdef _DEBUG
    if (!g_checkBoneOrder)
    {
        g_checkBoneOrder = 1;
        for (i = 0; i < 0x11; ++i)
        {
            iassert(strcmp(g_highPrecisionParts[i], g_highPrecisionParts[i + 1]) < 0);
        }
    }
#endif

    if (Com_sprintf(dest, 0x40u, "xanim/%s", name) < 0)
    {
        Com_PrintError(CON_CHANNEL_ANIM, (char *)"ERROR: filename '%s' too long\n", dest);
        return 0;
    }

    fileSize = FS_ReadFile(dest, (void **)&buf);

    if (fileSize < 0)
    {
        iassert(!buf);
        Com_PrintError(CON_CHANNEL_ANIM, (char *)"ERROR: xanim '%s' not found\n", name);
        return 0;
    }

    if (fileSize < 10)
    {
        Com_PrintError(CON_CHANNEL_ANIM, "ERROR: xanim '%s' has an incomplete header\n", name);
        FS_FreeFile((char *)buf);
        return 0;
    }

    iassert(buf);

    pos = buf;

    version = Buf_Read<short>(&pos);

    if (version != 17)
    {
        FS_FreeFile((char *)buf);
        Com_PrintError(CON_CHANNEL_ANIM, (char *)"ERROR: xanim '%s' out of date (version %d, expecting %d)\n", name, version, 17);
        return 0;
    }

    v69 = I_strnicmp(name, "viewmodel_", 10) == 0;
    parts = (XAnimParts *)Alloc(sizeof(XAnimParts));
    iassert(parts);
    memset(parts, 0, sizeof(XAnimParts));

    numframes = Buf_Read<ushort>(&pos);
    numBones = Buf_Read<short>(&pos);

    if (numBones < 0 || numBones > DOBJ_MAX_PARTS)
    {
        Com_Error(ERR_DROP, "XAnimLoadFile: invalid bone count %d", numBones);
    }

    parts->boneCount[9] = numBones;
    partFlags = Buf_Read<byte>(&pos);
    parts->bLoop = (partFlags & 1) != 0;
    parts->bDelta = (partFlags & 2) != 0;
    parts->assetType = Buf_Read<byte>(&pos);
    parts->framerate = (float)Buf_Read<short>(&pos);

    if (!numframes || (parts->bLoop && numframes == UINT16_MAX))
    {
        Com_Error(ERR_DROP, "XAnimLoadFile: invalid frame count %u", numframes);
    }

    if (parts->bLoop)
    {
        totalframes = numframes + 1;
    }
    else
    {
        totalframes = numframes;
    }

    numLoopFrames = totalframes;
    useSmallIndices = totalframes <= 0x100u;
    parts->numframes = totalframes - 1;
    if (parts->numframes)
    {
        parts->frequency = parts->framerate / (double)parts->numframes;
    }
    else
    {
        parts->frequency = 0.0;
    }

    iassert(parts->frequency >= 0);

    if (parts->bDelta)
    {
        parts->deltaPart = (XAnimDeltaPart *)Alloc(sizeof(XAnimDeltaPart));
        pos = GetDeltaQuaternions(parts->deltaPart, Alloc, pos, numLoopFrames, useSmallIndices);
        pos = GetDeltaTranslations(parts->deltaPart, Alloc, pos, numLoopFrames, useSmallIndices);
    }
    if (numBones)
    {
        count = ((uint)(numBones - 1) >> 3) + 1;
        v63 = pos;
        pos += count;
        memcpy(dst, pos, count);
        pos += count;
        memset(part, 0, sizeof(XAnimPartQuatPtr) * numBones);
        memset(v73, 0, sizeof(XAnimPartTransPtr) * numBones);
        for (i = 0; i < numBones; ++i)
        {
            part[i].partIndex = i;
            v73[i].partIndex = i;
        }
    }
    else
    {
        v63 = 0;
    }
    for (i = 0; i < numBones; ++i)
    {
        count = strlen((const char *)pos) + 1;
        prev = SL_GetStringOfSize((char *)pos, 0, count, MT_TYPE_ANIM_PART);
        v68[i] = prev;
        v27 = v69 || XAnimIsHighPrecisionPart((const char *)pos);
        g_highPrecisionPart[i] = v27;
        pos += count;
    }

    iassert(!g_animUser);

    g_animUser = Hunk_UserCreate(0x20000, "XAnimLoadFile", 0, 0, 11);
    for (i = 0; i < numBones; ++i)
    {
        Quaternions = GetQuaternions(
            &part[i],
            pos,
            ((1 << (i & 7)) & v63[i >> 3]) != 0,
            ((1 << (i & 7)) & (char)dst[i >> 3]) != 0,
            numLoopFrames,
            useSmallIndices);
        pos = GetTranslations(&v73[i], Quaternions, numLoopFrames, useSmallIndices);
    }
    ReadNoteTracks(name, &pos, parts, Alloc);

    iassert(buf + fileSize == pos);

    FS_FreeFile((char *)buf);
    if (numBones)
    {
        for (i = 0; i < numBones; ++i)
        {
            base[i] = i;
        }
        g_partQuatArray = part;
        g_partTransArray = v73;
        g_simpleQuatBits = (char *)dst;
        parts->names = (uint16_t *)Alloc(2 * numBones);
        memset(v51, 0, sizeof(v51));
        qsort(base, numBones, 2u, XAnimCompareQuatParts);
        for (i = 0; i < numBones; ++i)
        {
            animPartIndex = (uint16_t)base[i];
            part[animPartIndex].partIndex = i;
            v73[animPartIndex].partIndex = i;
            sortedQuats[i] = part[animPartIndex];
            parts->names[i] = v68[animPartIndex];
            PartQuatType = XAnimGetPartQuatType(animPartIndex);
            v51[PartQuatType] = i + 1;
        }
        if (v51[1] < v51[0])
        {
            v26 = v51[0];
        }
        else
        {
            v26 = v51[1];
        }
        v51[1] = v26;
        if (v51[2] < v26)
        {
            v25 = v51[1];
        }
        else
        {
            v25 = v51[2];
        }
        v51[2] = v25;
        if (v51[3] < v25)
        {
            v24 = v51[2];
        }
        else
        {
            v24 = v51[3];
        }
        v51[3] = v24;
        if (v51[4] < v24)
        {
            v23 = v51[3];
        }
        else
        {
            v23 = v51[4];
        }
        v51[4] = v23;
        v34 = v51[0];

        if (v51[0] != LOBYTE(v51[0]))
        {
            MyAssertHandler(
                (char *)"c:\\trees\\cod3\\src\\qcommon\\../universal/assertive.h",
                281,
                0,
                "i == static_cast< Type >( i )\n\t%i, %i",
                v51[0],
                LOBYTE(v51[0]));
        }
        parts->boneCount[0] = v34;

        v33 = v51[1] - v51[0];
        if (v51[1] - v51[0] != (unsigned __int8)(LOBYTE(v51[1]) - LOBYTE(v51[0])))
        {
            MyAssertHandler(
                (char *)"c:\\trees\\cod3\\src\\qcommon\\../universal/assertive.h",
                281,
                0,
                "i == static_cast< Type >( i )\n\t%i, %i",
                v33,
                (unsigned __int8)v33);
        }
        parts->boneCount[1] = v33;

        v32 = v51[2] - v51[1];
        if (v51[2] - v51[1] != (unsigned __int8)(LOBYTE(v51[2]) - LOBYTE(v51[1])))
        {
            MyAssertHandler(
                (char *)"c:\\trees\\cod3\\src\\qcommon\\../universal/assertive.h",
                281,
                0,
                "i == static_cast< Type >( i )\n\t%i, %i",
                v32,
                (unsigned __int8)v32);
        }
        parts->boneCount[2] = v32;

        v31 = v51[3] - v51[2];
        if (v51[3] - v51[2] != (unsigned __int8)(LOBYTE(v51[3]) - LOBYTE(v51[2])))
        {
            MyAssertHandler(
                (char *)"c:\\trees\\cod3\\src\\qcommon\\../universal/assertive.h",
                281,
                0,
                "i == static_cast< Type >( i )\n\t%i, %i",
                v31,
                (unsigned __int8)v31);
        }
        parts->boneCount[3] = v31;

        v30 = v51[4] - v51[3];
        if (v51[4] - v51[3] != (unsigned __int8)(LOBYTE(v51[4]) - LOBYTE(v51[3])))
        {
            MyAssertHandler(
                (char *)"c:\\trees\\cod3\\src\\qcommon\\../universal/assertive.h",
                281,
                0,
                "i == static_cast< Type >( i )\n\t%i, %i",
                v30,
                (unsigned __int8)v30);
        }
        parts->boneCount[4] = v30;

        memcpy(boneIndexes, (unsigned __int8 *)base, 2 * numBones);
        qsort(boneIndexes, numBones, 2, XAnimCompareTransParts);
        for (i = 0; i < numBones; ++i)
        {
            // animPartIndex = *(uint16_t *)&v72[2 * i];
            animPartIndex = boneIndexes[i];
            sortedTrans[i] = v73[animPartIndex];
            PartQuatType = XAnimGetPartTransType(animPartIndex);
            v51[PartQuatType] = i + 1;
        }
        if (v51[6] < v51[5])
        {
            v22 = v51[5];
        }
        else
        {
            v22 = v51[6];
        }
        v51[6] = v22;
        if (v51[7] < v22)
        {
            v21 = v51[6];
        }
        else
        {
            v21 = v51[7];
        }
        v51[7] = v21;
        if (v51[8] < v21)
        {
            v20 = v51[7];
        }
        else
        {
            v20 = v51[8];
        }
        v51[8] = v20;
        parts->boneCount[5] = v51[5];
        parts->boneCount[6] = LOBYTE(v51[6]) - LOBYTE(v51[5]);
        parts->boneCount[7] = LOBYTE(v51[7]) - LOBYTE(v51[6]);
        parts->boneCount[8] = LOBYTE(v51[8]) - LOBYTE(v51[7]);
        dataByteCount = 0;
        dataShortCount = 0;
        dataIntCount = 0;
        randomDataByteCount = 0;
        randomDataShortCount = 0;
        randomDataIntCount = 0;
        indexCount = 0;
        animPartIndex = 0;
        v56 = parts->boneCount[PART_TYPE_NO_QUAT];
        animPartIndex = parts->boneCount[PART_TYPE_NO_QUAT];

        v56 += parts->boneCount[PART_TYPE_HALF_QUAT];

        while (animPartIndex < v56)
        {
            tableSize = sortedQuats[animPartIndex].quat->size;
            ++dataShortCount;
            if (useSmallIndices)
            {
                dataByteCount += tableSize + 1;
            }
            else if (tableSize >= 0x40)
            {
                dataShortCount += ((tableSize - 1) >> 8) + 1 + 1;
                indexCount += tableSize + 1;
            }
            else
            {
                dataShortCount += tableSize + 1;
            }
            ++animPartIndex;
            randomDataShortCount += 2 * tableSize + 2;
        }

        v56 += parts->boneCount[PART_TYPE_FULL_QUAT];
        while (animPartIndex < v56)
        {
            tableSize = sortedQuats[animPartIndex].quat->size;
            ++dataShortCount;
            if (useSmallIndices)
            {
                dataByteCount += tableSize + 1;
            }
            else if (tableSize >= 0x40)
            {
                dataShortCount += ((tableSize - 1) >> 8) + 1 + 1;
                indexCount += tableSize + 1;
            }
            else
            {
                dataShortCount += tableSize + 1;
            }
            ++animPartIndex;
            randomDataShortCount += 4 * tableSize + 4;
        }

        v56 += parts->boneCount[PART_TYPE_HALF_QUAT_NO_SIZE];
        animPartIndex += parts->boneCount[PART_TYPE_HALF_QUAT_NO_SIZE];
        dataShortCount += 2 * parts->boneCount[PART_TYPE_HALF_QUAT_NO_SIZE];

        v56 += parts->boneCount[PART_TYPE_FULL_QUAT_NO_SIZE];
        animPartIndex += parts->boneCount[PART_TYPE_FULL_QUAT_NO_SIZE];
        dataShortCount += 4 * parts->boneCount[PART_TYPE_FULL_QUAT_NO_SIZE];

        iassert(animPartIndex == parts->boneCount[PART_TYPE_ALL]);

        animPartIndex = 0;
        v56 = parts->boneCount[PART_TYPE_SMALL_TRANS];
        dataByteCount += parts->boneCount[PART_TYPE_SMALL_TRANS];
        dataIntCount += 6 * parts->boneCount[PART_TYPE_SMALL_TRANS];

        while (animPartIndex < v56)
        {
            tableSize = sortedTrans[animPartIndex].trans->size;
            ++dataShortCount;
            if (useSmallIndices)
            {
                dataByteCount += tableSize + 1;
            }
            else if (tableSize >= 0x40)
            {
                dataShortCount += ((tableSize - 1) >> 8) + 1 + 1;
                indexCount += tableSize + 1;
            }
            else
            {
                dataShortCount += tableSize + 1;
            }
            ++animPartIndex;
            randomDataByteCount += 3 * (tableSize + 1);
        }
        v56 += parts->boneCount[PART_TYPE_TRANS];
        dataByteCount += parts->boneCount[PART_TYPE_TRANS];
        dataIntCount += 6 * parts->boneCount[PART_TYPE_TRANS];
        while (animPartIndex < v56)
        {
            tableSize = sortedTrans[animPartIndex].trans->size;
            ++dataShortCount;
            if (useSmallIndices)
            {
                dataByteCount += tableSize + 1;
            }
            else if (tableSize >= 0x40)
            {
                dataShortCount += ((tableSize - 1) >> 8) + 1 + 1;
                indexCount += tableSize + 1;
            }
            else
            {
                dataShortCount += tableSize + 1;
            }
            ++animPartIndex;
            randomDataShortCount += 3 * (tableSize + 1);
        }
        v56 += parts->boneCount[PART_TYPE_TRANS_NO_SIZE];
        animPartIndex += parts->boneCount[PART_TYPE_TRANS_NO_SIZE];
        dataByteCount += parts->boneCount[PART_TYPE_TRANS_NO_SIZE];
        dataIntCount += 3 * parts->boneCount[PART_TYPE_TRANS_NO_SIZE];

        v56 += parts->boneCount[PART_TYPE_NO_TRANS];
        animPartIndex += parts->boneCount[PART_TYPE_NO_TRANS];
        dataByteCount += parts->boneCount[PART_TYPE_NO_TRANS];

        iassert(animPartIndex == parts->boneCount[PART_TYPE_ALL]);

        if (dataByteCount > UINT16_MAX || dataShortCount > UINT16_MAX || dataIntCount > UINT16_MAX || randomDataByteCount > UINT16_MAX || randomDataIntCount > UINT16_MAX)
        {
            Com_Error(ERR_DROP, "XAnimLoadFile: animation data exceeds count fields");
        }
        if (dataByteCount)
        {
            v19 = (unsigned __int8 *)Alloc(dataByteCount);
        }
        else
        {
            v19 = 0;
        }
        dataByte = v19;
        parts->dataByteCount = dataByteCount;
        parts->dataByte = dataByte;
        if (dataShortCount)
        {
            v18 = (__int16 *)Alloc(2 * dataShortCount);
        }
        else
        {
            v18 = 0;
        }
        dataShort = v18;
        parts->dataShortCount = dataShortCount;
        parts->dataShort = dataShort;
        if (dataIntCount)
        {
            v17 = (int *)Alloc(4 * dataIntCount);
        }
        else
        {
            v17 = 0;
        }
        dataInt = v17;
        parts->dataIntCount = dataIntCount;
        parts->dataInt = dataInt;
        if (randomDataByteCount)
        {
            v16 = (unsigned __int8 *)Alloc(randomDataByteCount);
        }
        else
        {
            v16 = 0;
        }
        randomDataByte = v16;
        parts->randomDataByteCount = randomDataByteCount;
        parts->randomDataByte = randomDataByte;
        if (randomDataShortCount)
        {
            v15 = (unsigned __int8 *)Alloc(2 * randomDataShortCount);
        }
        else
        {
            v15 = 0;
        }
        randomDataShort = v15;
        parts->randomDataShortCount = randomDataShortCount;
        parts->randomDataShort = (__int16 *)randomDataShort;
        if (randomDataIntCount)
        {
            v14 = (int *)Alloc(4 * randomDataIntCount);
        }
        else
        {
            v14 = 0;
        }
        randomDataInt = v14;
        parts->randomDataIntCount = randomDataIntCount;
        parts->randomDataInt = randomDataInt;
        if (useSmallIndices)
        {
            if (indexCount)
            {
                v13 = Alloc(indexCount);
            }
            else
            {
                v13 = 0;
            }
            indices._1 = (unsigned __int8 *)v13;
        }
        else
        {
            if (indexCount)
            {
                v12 = Alloc(2 * indexCount);
            }
            else
            {
                v12 = 0;
            }
            indices._1 = (unsigned __int8 *)v12;
        }
        parts->indices = indices;
        parts->indexCount = indexCount;
        animPartIndex = 0;
        for (uint type = PART_TYPE_NO_QUAT; type <= PART_TYPE_FULL_QUAT_NO_SIZE; ++type)
        {
            for (uint bone = 0; bone < parts->boneCount[type]; ++bone, ++animPartIndex)
            {
                const XAnimPartQuat *quat = sortedQuats[animPartIndex].quat;
                if (type == PART_TYPE_NO_QUAT)
                {
                    continue;
                }
                if (type == PART_TYPE_HALF_QUAT || type == PART_TYPE_FULL_QUAT)
                {
                    tableSize = quat->size;
                    *dataShort++ = tableSize;
                    uint bytes = (type == PART_TYPE_HALF_QUAT ? sizeof(int16_t[2]) : sizeof(int16_t[4])) * (tableSize + 1);
                    memcpy(randomDataShort, quat->u.frames.u.frames, bytes);
                    randomDataShort += bytes;
                    XAnimEmitFrameIndices(tableSize, &dataByte, &dataShort, &indices,
                                          (XAnimDynamicIndices *)&quat->u.frames.indices, useSmallIndices);
                }
                else
                {
                    uint shorts = type == PART_TYPE_HALF_QUAT_NO_SIZE ? 2 : 4;
                    memcpy(dataShort, quat->u.frame0, sizeof(int16_t) * shorts);
                    dataShort += shorts;
                }
            }
        }
        iassert(animPartIndex == parts->boneCount[PART_TYPE_ALL]);

        animPartIndex = 0;
        for (uint type = PART_TYPE_SMALL_TRANS; type <= PART_TYPE_NO_TRANS; ++type)
        {
            for (uint bone = 0; bone < parts->boneCount[type]; ++bone, ++animPartIndex)
            {
                const XAnimPartTrans *trans = sortedTrans[animPartIndex].trans;
                *dataByte++ = sortedTrans[animPartIndex].partIndex;
                if (type == PART_TYPE_NO_TRANS)
                {
                    continue;
                }
                if (type == PART_TYPE_TRANS_NO_SIZE)
                {
                    memcpy(dataInt, trans->u.frame0, sizeof(float[3]));
                    dataInt += 3;
                    continue;
                }
                tableSize = trans->size;
                *dataShort++ = tableSize;
                memcpy(dataInt, trans->u.frames.mins, sizeof(float[3]));
                memcpy(dataInt + 3, trans->u.frames.size, sizeof(float[3]));
                dataInt += 6;
                if (type == PART_TYPE_SMALL_TRANS)
                {
                    uint bytes = sizeof(byte[3]) * (tableSize + 1);
                    memcpy(randomDataByte, trans->u.frames.frames._1, bytes);
                    randomDataByte += bytes;
                }
                else
                {
                    uint bytes = sizeof(uint16_t[3]) * (tableSize + 1);
                    memcpy(randomDataShort, trans->u.frames.frames._2, bytes);
                    randomDataShort += bytes;
                }
                XAnimEmitFrameIndices(tableSize, &dataByte, &dataShort, &indices,
                                      (XAnimDynamicIndices *)&trans->u.frames.indices, useSmallIndices);
            }
        }
        iassert(animPartIndex == parts->boneCount[PART_TYPE_ALL]);

        if (useSmallIndices)
        {
            iassert(indices._1 == &parts->indices._1[indexCount]);
        }
        else
        {
            iassert(indices._2 == &parts->indices._2[indexCount]);
        }

        iassert(dataByte == &parts->dataByte[dataByteCount]);
        iassert(dataShort == &parts->dataShort[dataShortCount]);
        iassert(dataInt == &parts->dataInt[dataIntCount]);
        iassert(randomDataByte == &parts->randomDataByte[randomDataByteCount]);
        iassert(randomDataShort == (byte *)&parts->randomDataShort[randomDataShortCount]);
        iassert(randomDataInt == &parts->randomDataInt[randomDataIntCount]);
        iassert(dataByteCount * sizeof(byte) <= 16 * 1024);
        iassert(dataShortCount * sizeof(ushort) <= 16 * 1024);
        iassert(dataIntCount * sizeof(int) <= 16 * 1024);
    }

    iassert(g_animUser);
    Hunk_UserDestroy(g_animUser);
    g_animUser = 0;
    return parts;
}

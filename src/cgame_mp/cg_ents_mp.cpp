#ifndef KISAK_MP
#error This File is MultiPlayer Only
#endif

#include <universal/q_shared.h>
#include "cg_local_mp.h"

#include <aim_assist/aim_assist.h>

#include <EffectsCore/fx_system.h>

#include <gfx_d3d/r_scene.h>
#include <gfx_d3d/r_workercmds_common.h>

#include <script/scr_const.h>

#include <physics/phys_local.h>

#include <ragdoll/ragdoll.h>

#include <qcommon/com_bsp.h>

#include <win32/win_local.h>

#include <xanim/dobj.h>
#include <xanim/dobj_utils.h>
#include <script/scr_memorytree.h>
#include <game_mp/g_public_mp.h>
#include <universal/profile.h>

float g_entMoveTolVec[3] = { 16.0f, 16.0f, 16.0f };

void __cdecl CG_Player_PreControllers(DObj_s *obj, centity_s *cent)
{
    clientInfo_t *ci; // [esp+Ch] [ebp-8h]
    int i; // [esp+10h] [ebp-4h]
    cg_s *cgameGlob;

    cgameGlob = CG_GetLocalClientGlobals(cent->pose.localClientNum);
    cent->pose.eType = cent->nextState.eType;
    iassert(cent->pose.eType == cent->nextState.eType);
    bcassert(cent->nextState.clientNum, MAX_CLIENTS);
    ci = &cgameGlob->bgs.clientinfo[cent->nextState.clientNum];
    if (ci->infoValid)
    {
        BG_Player_DoControllersSetup(&cent->nextState, ci, cgameGlob->frametime);
        for (i = 0; i < 6; ++i)
            DObjGetBoneIndex(obj, *controller_names[i], &cent->pose.player.tag[i]);
        cent->pose.player.control = &ci->control;
    }
    else
    {
        cent->pose.player.control = NULL;
    }
}

void __cdecl CG_mg42_PreControllers(DObj_s *obj, centity_s *cent)
{
    float v2; // [esp+14h] [ebp-80h]
    float v3; // [esp+18h] [ebp-7Ch]
    float v4; // [esp+1Ch] [ebp-78h]
    float v5; // [esp+20h] [ebp-74h]
    float v6; // [esp+24h] [ebp-70h]
    float v7; // [esp+28h] [ebp-6Ch]
    float v8; // [esp+2Ch] [ebp-68h]
    float v9; // [esp+30h] [ebp-64h]
    float v10; // [esp+34h] [ebp-60h]
    float v11; // [esp+38h] [ebp-5Ch]
    float v12; // [esp+3Ch] [ebp-58h]
    float v13; // [esp+40h] [ebp-54h]
    bool v14; // [esp+44h] [ebp-50h]
    float v15; // [esp+48h] [ebp-4Ch]
    float v16; // [esp+50h] [ebp-44h]
    float v17; // [esp+54h] [ebp-40h]
    float v18; // [esp+58h] [ebp-3Ch]
    float v19; // [esp+60h] [ebp-34h]
    float v20; // [esp+64h] [ebp-30h]
    float v21; // [esp+68h] [ebp-2Ch]
    float frameInterpolation; // [esp+70h] [ebp-24h]
    float v23; // [esp+74h] [ebp-20h]
    uint playAnim; // [esp+7Ch] [ebp-18h]
    const cg_s *cgameGlob;

    iassert(cent->nextState.eType == ET_MG42);

    cgameGlob = CG_GetLocalClientGlobals(cent->pose.localClientNum);
    v14 = (cgameGlob->predictedPlayerState.eFlags & 0x300) != 0
        && cgameGlob->predictedPlayerState.viewlocked_entNum == cent->nextState.number;
    cent->pose.turret.playerUsing = v14;
    if (cent->pose.turret.playerUsing)
    {
        cent->pose.turret.viewAngles = cgameGlob->refdefViewAngles;
        cent->pose.cullIn = 0;
    }
    else
    {
        v21 = cent->currentState.u.turret.gunAngles[0];
        frameInterpolation = cgameGlob->frameInterpolation;
        v13 = cent->nextState.lerp.u.turret.gunAngles[0] - v21;
        v23 = v13 * 0.002777777845039964f;
        v12 = v23 + 0.5f;
        v11 = floor(v12);
        v10 = (v23 - v11) * 360.0f;
        cent->pose.turret.angles.pitch = v10 * frameInterpolation + v21;
        v18 = cent->currentState.u.turret.gunAngles[1];
        v19 = cgameGlob->frameInterpolation;
        v9 = cent->nextState.lerp.u.turret.gunAngles[1] - v18;
        v20 = v9 * 0.002777777845039964f;
        v8 = v20 + 0.5f;
        v7 = floor(v8);
        v6 = (v20 - v7) * 360.0f;
        cent->pose.turret.angles.yaw = v6 * v19 + v18;
    }
    v15 = cent->currentState.u.turret.gunAngles[2];
    v16 = cgameGlob->frameInterpolation;
    v5 = cent->nextState.lerp.u.turret.gunAngles[2] - v15;
    v17 = v5 * 0.002777777845039964f;
    v4 = v17 + 0.5f;
    v3 = floor(v4);
    v2 = (v17 - v3) * 360.0f;
    cent->pose.turret.barrelPitch = v2 * v16 + v15;
    DObjGetBoneIndex(obj, scr_const.tag_aim, &cent->pose.turret.tag_aim);
    DObjGetBoneIndex(obj, scr_const.tag_aim_animated, &cent->pose.turret.tag_aim_animated);
    DObjGetBoneIndex(obj, scr_const.tag_flash, &cent->pose.turret.tag_flash);
    DObjGetTree(obj);
    if ((cgameGlob->predictedPlayerState.eFlags & 0x300) != 0
        && cgameGlob->predictedPlayerState.viewlocked_entNum == cent->nextState.number)
    {
        playAnim = 1;
    }
    else if ((cent->nextState.lerp.eFlags & 0x40) != 0)
    {
        playAnim = 2;
    }
    else
    {
        playAnim = 1;
    }
    XAnimSetGoalWeightKnobAll(obj, playAnim, 0, 1.0f, 0.1f, 1.0f, 0, 0, 0);
}

void  CG_UpdateBModelWorldBounds(uint localClientNum, centity_s *cent, int forceFilter)
{
    float axis[3][3];
    float mins[3];
    float maxs[3];
    GfxBrushModel *brush = R_GetBrushModel(cent->nextState.index.brushmodel);

    AnglesToAxis(cent->pose.angles, axis);
    for (int i = 0; i < 3; ++i)
    {
        mins[i] = cent->pose.origin[i];
        maxs[i] = cent->pose.origin[i];
        for (int j = 0; j < 3; ++j)
        {
            float minOffset = brush->bounds[0][j] * axis[j][i];
            float maxOffset = brush->bounds[1][j] * axis[j][i];
            if (axis[j][i] < 0.0f)
            {
                mins[i] += maxOffset;
                maxs[i] += minOffset;
            }
            else
            {
                mins[i] += minOffset;
                maxs[i] += maxOffset;
            }
        }
    }

    if (forceFilter || !CG_VecLessThan(brush->writable.mins, mins)
        || !CG_VecLessThan(maxs, brush->writable.maxs))
    {
        if (!forceFilter)
        {
            Vec3Sub(mins, g_entMoveTolVec, mins);
            Vec3Add(maxs, g_entMoveTolVec, maxs);
        }
        Vec3Copy(mins, brush->writable.mins);
        Vec3Copy(maxs, brush->writable.maxs);
        R_LinkBModelEntity(localClientNum, cent->nextState.number, brush);
    }
}

// KISAKTODO: Remove this stupid function
bool __cdecl CG_VecLessThan(float *a, float *b)
{
    float v3; // [esp+4h] [ebp-1Ch]
    float v4; // [esp+8h] [ebp-18h]
    float v5; // [esp+Ch] [ebp-14h]
    float v6; // [esp+10h] [ebp-10h]
    float v7; // [esp+14h] [ebp-Ch]
    float v8; // [esp+18h] [ebp-8h]
    float v9; // [esp+1Ch] [ebp-4h]

    v8 = *a - *b;
    v9 = a[1] - b[1];
    v5 = v8 - v9;
    if (v5 < 0.0)
        v6 = a[1] - b[1];
    else
        v6 = *a - *b;
    v7 = a[2] - b[2];
    v4 = v6 - v7;
    if (v4 < 0.0)
        v3 = a[2] - b[2];
    else
        v3 = v6;
    return v3 <= 0.0;
}

/*
=========================
CG_AdjustPositionForMover

Also called by client movement prediction code
=========================
*/
void __cdecl CG_AdjustPositionForMover(
    int localClientNum,
    const float *in,
    int moverNum,
    int fromTime,
    int toTime,
    float *out,
    float *outDeltaAngles)
{
    float origin[3]; // [esp+0h] [ebp-4Ch] BYREF
    centity_s *cent; // [esp+Ch] [ebp-40h]
    float angles[3]; // [esp+10h] [ebp-3Ch] BYREF
    float deltaAngles[3]; // [esp+1Ch] [ebp-30h] BYREF
    float oldOrigin[3]; // [esp+28h] [ebp-24h] BYREF
    float oldAngles[3]; // [esp+34h] [ebp-18h] BYREF
    float deltaOrigin[3]; // [esp+40h] [ebp-Ch] BYREF

    if (outDeltaAngles)
    {
        *outDeltaAngles = 0.0;
        outDeltaAngles[1] = 0.0;
        outDeltaAngles[2] = 0.0;
    }
    if (moverNum > 0 && moverNum < ENTITYNUM_WORLD)
    {
        cent = CG_GetEntity(localClientNum, moverNum);
        if (cent->nextState.eType == ET_SCRIPTMOVER || cent->nextState.eType == ET_PLANE)
        {
            BG_EvaluateTrajectory(&cent->currentState.pos, fromTime, oldOrigin);
            BG_EvaluateTrajectory(&cent->currentState.apos, fromTime, oldAngles);
            BG_EvaluateTrajectory(&cent->currentState.pos, toTime, origin);
            BG_EvaluateTrajectory(&cent->currentState.apos, toTime, angles);
            Vec3Sub(origin, oldOrigin, deltaOrigin);
            Vec3Sub(angles, oldAngles, deltaAngles);
            Vec3Add(in, deltaOrigin, out);
            if (outDeltaAngles)
            {
                *outDeltaAngles = deltaAngles[0];
                outDeltaAngles[1] = deltaAngles[1];
                outDeltaAngles[2] = deltaAngles[2];
            }

            // FIXME: origin change when on a rotating object
        }
        else
        {
            *out = *in;
            out[1] = in[1];
            out[2] = in[2];
        }
    }
    else
    {
        *out = *in;
        out[1] = in[1];
        out[2] = in[2];
    }
}

void __cdecl CG_SetFrameInterpolation(int localClientNum)
{
    int delta; // [esp+4h] [ebp-8h]
    cg_s *cgameGlob;

    cgameGlob = CG_GetLocalClientGlobals(localClientNum);

    iassert(cgameGlob->snap);
    iassert(cgameGlob->nextSnap);

    delta = cgameGlob->nextSnap->serverTime - cgameGlob->snap->serverTime;
    if (delta)
    {
        cgameGlob->frameInterpolation = (double)(cgameGlob->time - cgameGlob->snap->serverTime) / (double)delta;
        if (cgameGlob->frameInterpolation < 0.0)
            cgameGlob->frameInterpolation = 0.0;
    }
    else
    {
        cgameGlob->frameInterpolation = 0.0;
    }
}

void __cdecl CG_ProcessClientNoteTracks(cg_s *cgameGlob, uint clientNum)
{
    XAnimNotify_s *noteList; // [esp+4h] [ebp-Ch] BYREF
    int i; // [esp+8h] [ebp-8h]
    int listSize; // [esp+Ch] [ebp-4h]

    if (clientNum < 0x40)
    {
        listSize = DObjGetClientNotifyList(&noteList);
        for (i = 0; i < listSize; ++i)
        {
            if (noteList[i].type == 1)
            {
                if (I_stricmp(noteList[i].name, "anim_gunhand = \"left\""))
                {
                    if (!I_stricmp(noteList[i].name, "anim_gunhand = \"right\""))
                    {
                        cgameGlob->bgs.clientinfo[clientNum].leftHandGun = 0;
                        cgameGlob->bgs.clientinfo[clientNum].dobjDirty = 1;
                    }
                }
                else
                {
                    cgameGlob->bgs.clientinfo[clientNum].leftHandGun = 1;
                    cgameGlob->bgs.clientinfo[clientNum].dobjDirty = 1;
                }
            }
        }
    }
}

void __cdecl CG_AddPacketEntity(int localClientNum, int entnum)
{
    bool v2; // [esp+4h] [ebp-84h]
    bool v4; // [esp+Ch] [ebp-7Ch]
    bool v5; // [esp+10h] [ebp-78h]
    bool v7; // [esp+18h] [ebp-70h]
    float *v8; // [esp+24h] [ebp-64h]
    float *v9; // [esp+28h] [ebp-60h]
    float diff[3]; // [esp+2Ch] [ebp-5Ch] BYREF
    float *v11; // [esp+38h] [ebp-50h]
    float *v12; // [esp+3Ch] [ebp-4Ch]
    float *v13; // [esp+40h] [ebp-48h]
    float *v14; // [esp+44h] [ebp-44h]
    float *v15; // [esp+48h] [ebp-40h]
    float radius; // [esp+4Ch] [ebp-3Ch]
    DObj_s *obj; // [esp+50h] [ebp-38h]
    float newAngles[3]; // [esp+54h] [ebp-34h] BYREF
    int vehSlot; // [esp+60h] [ebp-28h]
    float origin[3]; // [esp+64h] [ebp-24h]
    centity_s *cent; // [esp+70h] [ebp-18h]
    float angles[3]; // [esp+74h] [ebp-14h]
    bool entMoved; // [esp+83h] [ebp-5h]
    uint eType; // [esp+84h] [ebp-4h]

    cent = CG_GetEntity(localClientNum, entnum);
    eType = cent->nextState.eType;
    v15 = cent->pose.origin;
    origin[0] = cent->pose.origin[0];
    origin[1] = cent->pose.origin[1];
    origin[2] = cent->pose.origin[2];
    v14 = cent->pose.angles;
    angles[0] = cent->pose.angles[0];
    angles[1] = cent->pose.angles[1];
    angles[2] = cent->pose.angles[2];
    entMoved = 0;
    if ((eType == ET_SCRIPTMOVER || eType == ET_PLANE) && cent->nextState.solid == 0xFFFFFF)
    {
        CG_CalcEntityLerpPositions(localClientNum, cent);
        v4 = origin[0] == cent->pose.origin[0] && origin[1] == cent->pose.origin[1] && origin[2] == cent->pose.origin[2];
        v2 = 1;
        if (v4)
        {
            if (angles[0] == cent->pose.angles[0] && angles[1] == cent->pose.angles[1] && angles[2] == cent->pose.angles[2])
                v2 = 0;
        }
        entMoved = v2;
        if (v2)
            CG_UpdateBModelWorldBounds(localClientNum, cent, 0);
    }
    else
    {
        if (eType == ET_PLAYER && CG_VehEntityUsingVehicle(localClientNum, entnum))
        {
            CG_VehSeatTransformForPlayer(localClientNum, entnum, cent->pose.origin, newAngles);
            vehSlot = CG_VehPlayerVehicleSlot(localClientNum, entnum);
            if (vehSlot != 1)
            {
                v13 = cent->pose.angles;
                cent->pose.angles[0] = newAngles[0];
                v13[1] = newAngles[1];
                v13[2] = newAngles[2];
            }
        }
        else
        {
            CG_CalcEntityLerpPositions(localClientNum, cent);
        }
        v12 = cent->pose.origin;
        v7 = origin[0] == cent->pose.origin[0] && origin[1] == v12[1] && origin[2] == v12[2];
        v5 = 1;
        if (v7)
        {
            v11 = cent->pose.angles;
            if (angles[0] == cent->pose.angles[0] && angles[1] == v11[1] && angles[2] == v11[2])
                v5 = 0;
        }
        entMoved = v5;
        Vec3Sub(cg_entityOriginArray[localClientNum][entnum], cent->pose.origin, diff);
        if (Vec3LengthSq(diff) > 256.0)
        {
            v8 = cg_entityOriginArray[localClientNum][entnum];
            v9 = cent->pose.origin;
            *v8 = cent->pose.origin[0];
            v8[1] = v9[1];
            v8[2] = v9[2];
            obj = Com_GetClientDObj(entnum, localClientNum);
            if (obj)
            {
                radius = DObjGetRadius(obj) + 16.0;
                R_LinkDObjEntity(localClientNum, entnum, cent->pose.origin, radius);
            }
        }
    }
    if (cent->pose.physObjId == -1)
    {
        if (CG_IsEntityLinked(localClientNum, entnum))
            CG_UnlinkEntity(localClientNum, entnum);
    }
    else
    {
        if (CG_IsEntityLinked(localClientNum, entnum))
        {
            if (entMoved)
                CG_LinkEntity(localClientNum, entnum);
        }
        else if (CG_EntityNeedsLinked(localClientNum, entnum))
        {
            CG_LinkEntity(localClientNum, entnum);
        }
        CG_UpdateClientDobjPartBits(cent, entnum, localClientNum);
        CG_ProcessEntity(localClientNum, cent);
    }
}

void __cdecl CG_UpdateClientDobjPartBits(centity_s *cent, int entnum, int localClientNum)
{
    DObj_s *obj; // [esp+0h] [ebp-14h]
    uint oldPartBits[4]; // [esp+4h] [ebp-10h] BYREF

    iassert(cent);
    obj = Com_GetClientDObj(entnum, localClientNum);
    if (obj)
    {
        DObjGetHidePartBits(obj, oldPartBits);
        DObjSetHidePartBits(obj, cent->nextState.partBits);
        FX_MarkEntUpdateHidePartBits(oldPartBits, cent->nextState.partBits, localClientNum, entnum);
    }
}

/*
===============
CG_AddPacketEntities

===============
*/
int __cdecl CG_AddPacketEntities(int localClientNum)
{
    int viewlocked_entNum; // [esp+0h] [ebp-154h]
    uint linkedPlayerCount; // [esp+34h] [ebp-120h]
    int lockedView; // [esp+38h] [ebp-11Ch]
    centity_s *cent; // [esp+40h] [ebp-114h]
    int linkedPlayers[64]; // [esp+44h] [ebp-110h]
    int num; // [esp+144h] [ebp-10h]
    int entnum; // [esp+148h] [ebp-Ch]
    int lockedViewEntNum; // [esp+14Ch] [ebp-8h]
    uint eType; // [esp+150h] [ebp-4h]
    cg_s *cgameGlob;

    KISAK_NULLSUB();

    PROF_SCOPED("CG_AddPacketEntities");

    cgameGlob = CG_GetLocalClientGlobals(localClientNum);

    cgameGlob->rumbleScale = 0.0;
    if ((cgameGlob->predictedPlayerState.eFlags & 0x300) != 0)
        viewlocked_entNum = cgameGlob->predictedPlayerState.viewlocked_entNum;
    else
        viewlocked_entNum = ENTITYNUM_NONE;
    lockedViewEntNum = viewlocked_entNum;
    lockedView = 0;
    linkedPlayerCount = 0;
    CG_AddClientSideSounds(localClientNum);
    // add each entity sent over by the server
    for (num = 0; num < cgameGlob->nextSnap->numEntities; ++num)
    {
        entnum = cgameGlob->nextSnap->entities[num].number;
        cent = CG_GetEntity(localClientNum, entnum);
        eType = cent->nextState.eType;
        if (eType < ET_EVENTS)
        {
            if (entnum == lockedViewEntNum)
            {
                lockedView = 1;
                CG_CalcEntityLerpPositions(localClientNum, cent);
            }
            else if (eType == ET_PLAYER && CG_VehEntityUsingVehicle(localClientNum, entnum))
            {
                bcassert(linkedPlayerCount, MAX_CLIENTS);
                linkedPlayers[linkedPlayerCount++] = entnum;
            }
            else
            {
                CG_AddPacketEntity(localClientNum, entnum);
            }
        }
    }
    for (num = 0; num < (int)linkedPlayerCount; ++num)
    {
        entnum = linkedPlayers[num];
        CG_GetEntity(localClientNum, entnum);
        CG_AddPacketEntity(localClientNum, entnum);
    }
    return lockedView;
}

void __cdecl CG_DObjUpdateInfo(const cg_s *cgameGlob, DObj_s *obj, bool notify)
{
    float dtime; // [esp+8h] [ebp-4h]

    dtime = (double)cgameGlob->frametime * EQUAL_EPSILON;
    DObjUpdateClientInfo(obj, dtime, notify);
}

int __cdecl CG_DObjGetWorldBoneMatrix(
    const cpose_t *pose,
    DObj_s *obj,
    int boneIndex,
    float (*tagMat)[3],
    float *origin)
{
    DObjAnimMat *mat; // [esp+54h] [ebp-4h]

    iassert(obj);
    iassert(pose);

    mat = CG_DObjGetLocalBoneMatrix(pose, obj, boneIndex);
    if (!mat)
        return 0;

    ConvertQuatToMat(mat, tagMat);

    iassert(pose->localClientNum == 0);
    Vec3Add(mat->trans, CG_GetLocalClientGlobals(0)->refdef.viewOffset, origin);
    return 1;
}

DObjAnimMat *__cdecl CG_DObjGetLocalBoneMatrix(const cpose_t *pose, DObj_s *obj, int boneIndex)
{
    DObjAnimMat *mat; // [esp+34h] [ebp-4h]

    iassert(obj);
    {
        PROF_SCOPED("CG_DObjGetLocalTagMatrix");
        CG_DObjCalcBone(pose, obj, boneIndex);
    }
    mat = DObjGetRotTransArray(obj);
    if (mat)
        return &mat[boneIndex];
    else
        return 0;
}

int __cdecl CG_DObjGetWorldTagMatrix(
    const cpose_t *pose,
    DObj_s *obj,
    uint tagName,
    float (*tagMat)[3],
    float *origin)
{
    DObjAnimMat *mat; // [esp+54h] [ebp-4h]

    iassert(obj);
    iassert(pose);

    mat = CG_DObjGetLocalTagMatrix(pose, obj, tagName);
    if (!mat)
        return 0;

    ConvertQuatToMat(mat, tagMat);

    iassert(pose->localClientNum == 0);
    Vec3Add(mat->trans, CG_GetLocalClientGlobals(0)->refdef.viewOffset, origin);
    return 1;
}

DObjAnimMat *__cdecl CG_DObjGetLocalTagMatrix(const cpose_t *pose, DObj_s *obj, uint tagName)
{
    uint8_t boneIndex; // [esp+3h] [ebp-1h] BYREF

    iassert(obj);
    boneIndex = -2;

    if (DObjGetBoneIndex(obj, tagName, &boneIndex))
        return CG_DObjGetLocalBoneMatrix(pose, obj, boneIndex);
    else
        return 0;
}

int __cdecl CG_DObjGetWorldTagPos(const cpose_t *pose, DObj_s *obj, uint tagName, float *pos)
{
    DObjAnimMat *mat; // [esp+8h] [ebp-4h]

    iassert(obj);
    iassert(pose);

    mat = CG_DObjGetLocalTagMatrix(pose, obj, tagName);

    if (!mat)
        return 0;

    Vec3Add(mat->trans, CG_GetLocalClientGlobals(pose->localClientNum)->refdef.viewOffset, pos);

    return 1;
}

cpose_t*__cdecl CG_GetPose(int localClientNum, uint handle)
{
    iassert(handle >= 0 && handle < (((1 << 10)) + 128));

    if ((int)handle < 1024)
        return &CG_GetEntity(localClientNum, handle)->pose;

    iassert(handle >= ((1 << 10)) && handle - ((1 << 10)) < 128);

    return &CG_GetLocalClientGlobals(localClientNum)->viewModelPose;
}

/*
===============
CG_CalcEntityLerpPositions

===============
*/
void __cdecl CG_CalcEntityLerpPositions(int localClientNum, centity_s *cent)
{
    uint corpseIndex; // [esp+18h] [ebp-8h]
    clientInfo_t *ci; // [esp+1Ch] [ebp-4h]
    clientInfo_t *cia; // [esp+1Ch] [ebp-4h]
    cg_s *cgameGlob;
    cgs_t *cgs;

    cgameGlob = CG_GetLocalClientGlobals(localClientNum);

    if (cent->currentState.pos.trType == TR_PHYSICS)
    {
        CG_CalcEntityPhysicsPositions(localClientNum, cent);
    }
    else if (cent->currentState.pos.trType == TR_INTERPOLATE && cent->nextState.lerp.pos.trType != TR_PHYSICS
        || cent->currentState.pos.trType == TR_LINEAR_STOP && cent->nextState.number < 64)
    {
        CG_InterpolateEntityPosition(cgameGlob, cent);
    }
    else
    {
        BG_EvaluateTrajectory(&cent->currentState.pos, cgameGlob->time, cent->pose.origin);
        BG_EvaluateTrajectory(&cent->currentState.apos, cgameGlob->time, cent->pose.angles);
        if (cent->nextState.eType == ET_PLAYER)
        {
            bcassert(cent->nextState.clientNum, MAX_CLIENTS);

            ci = &cgameGlob->bgs.clientinfo[cent->nextState.clientNum];
            ci->lerpMoveDir = (float)cent->nextState.lerp.u.player.movementDir;
            ci->playerAngles[0] = cent->pose.angles[0];
            ci->playerAngles[1] = cent->pose.angles[1];
            ci->playerAngles[2] = cent->pose.angles[2];
            cent->pose.angles[0] = 0.0f;
            cent->pose.angles[2] = 0.0f;
            ci->lerpLean = cent->nextState.lerp.u.player.leanf;
        }
        else if (cent->nextState.eType == ET_PLAYER_CORPSE)
        {
            corpseIndex = cent->nextState.number - 64;
            iassert((unsigned)corpseIndex < MAX_CLIENT_CORPSES);
            cgs = CG_GetLocalClientStaticGlobals(localClientNum);
            cia = &cgs->corpseinfo[corpseIndex];
            cgs->corpseinfo[corpseIndex].lerpMoveDir = (float)cent->nextState.lerp.u.player.movementDir;
            cia->playerAngles[0] = cent->pose.angles[0];
            cia->playerAngles[1] = cent->pose.angles[1];
            cia->playerAngles[2] = cent->pose.angles[2];
            cent->pose.angles[0] = 0.0;
            cent->pose.angles[2] = 0.0;
            cia->lerpLean = cent->nextState.lerp.u.player.leanf;
        }
        if (cent != &cgameGlob->predictedPlayerEntity)
            CG_AdjustPositionForMover(
                localClientNum,
                cent->pose.origin,
                cent->nextState.groundEntityNum,
                cgameGlob->snap->serverTime,
                cgameGlob->time,
                cent->pose.origin,
                0);
        if (CG_IsRagdollTrajectory(&cent->currentState.pos))
            CG_CalcEntityRagdollPositions(localClientNum, cent);
    }
}

/*
=============================
CG_InterpolateEntityPosition
=============================
*/
void __cdecl CG_InterpolateEntityPosition(cg_s *cgameGlob, centity_s *cent)
{
    float v2; // [esp+8h] [ebp-B4h]
    float v3; // [esp+Ch] [ebp-B0h]
    float v4; // [esp+10h] [ebp-ACh]
    float v5; // [esp+14h] [ebp-A8h]
    float v6; // [esp+18h] [ebp-A4h]
    float v7; // [esp+1Ch] [ebp-A0h]
    float v8; // [esp+20h] [ebp-9Ch]
    float v9; // [esp+24h] [ebp-98h]
    float v10; // [esp+28h] [ebp-94h]
    float v11; // [esp+2Ch] [ebp-90h]
    float v12; // [esp+30h] [ebp-8Ch]
    float v13; // [esp+34h] [ebp-88h]
    float v14; // [esp+38h] [ebp-84h]
    float v15; // [esp+3Ch] [ebp-80h]
    float v16; // [esp+40h] [ebp-7Ch]
    float v17; // [esp+44h] [ebp-78h]
    float v18; // [esp+48h] [ebp-74h]
    float v19; // [esp+4Ch] [ebp-70h]
    float v20; // [esp+50h] [ebp-6Ch]
    float v21; // [esp+54h] [ebp-68h]
    float v22; // [esp+58h] [ebp-64h]
    float v23; // [esp+60h] [ebp-5Ch]
    float *playerAngles; // [esp+64h] [ebp-58h]
    float movementDir; // [esp+6Ch] [ebp-50h]
    float v26; // [esp+70h] [ebp-4Ch]
    float v27; // [esp+74h] [ebp-48h]
    float v28; // [esp+78h] [ebp-44h]
    float v29; // [esp+80h] [ebp-3Ch]
    float v30; // [esp+84h] [ebp-38h]
    float v31; // [esp+8Ch] [ebp-30h]
    float v32; // [esp+90h] [ebp-2Ch]
    float v33; // [esp+98h] [ebp-24h]
    float next[3]; // [esp+9Ch] [ebp-20h] BYREF
    float f; // [esp+A8h] [ebp-14h]
    clientInfo_t *ci; // [esp+ACh] [ebp-10h]
    float current[3]; // [esp+B0h] [ebp-Ch] BYREF

    iassert(cgameGlob->snap);
    // it would be an internal error to find an entity that interpolates without
    // a snapshot ahead of the current one
    iassert(cgameGlob->nextSnap);
    iassert(cent->nextState.lerp.pos.trType != TR_PHYSICS);
    f = cgameGlob->frameInterpolation;
    // this will linearize a sine or parabolic curve, but it is important
    // to not extrapolate player positions if more recent data is available
    BG_EvaluateTrajectory(&cent->currentState.pos, cgameGlob->snap->serverTime, current);
    BG_EvaluateTrajectory(&cent->nextState.lerp.pos, cgameGlob->nextSnap->serverTime, next);
    Vec3Lerp(current, next, f, cent->pose.origin);
    BG_EvaluateTrajectory(&cent->currentState.apos, cgameGlob->snap->serverTime, current);
    BG_EvaluateTrajectory(&cent->nextState.lerp.apos, cgameGlob->nextSnap->serverTime, next);
    v32 = current[0];
    v21 = next[0] - current[0];
    v33 = v21 * 0.002777777845039964;
    v20 = v33 + 0.5;
    v19 = floor(v20);
    v18 = (v33 - v19) * 360.0;
    cent->pose.angles[0] = v18 * f + v32;
    v30 = current[1];
    v17 = next[1] - current[1];
    v31 = v17 * 0.002777777845039964;
    v16 = v31 + 0.5;
    v15 = floor(v16);
    v14 = (v31 - v15) * 360.0;
    cent->pose.angles[1] = v14 * f + v30;
    v28 = current[2];
    v13 = next[2] - current[2];
    v29 = v13 * 0.002777777845039964;
    v12 = v29 + 0.5;
    v11 = floor(v12);
    v10 = (v29 - v11) * 360.0;
    cent->pose.angles[2] = v10 * f + v28;
    if (cent->nextState.eType == ET_PLAYER)
    {
        bcassert(cent->nextState.clientNum, 0x40u);
        ci = &cgameGlob->bgs.clientinfo[cent->nextState.clientNum];
        movementDir = (float)cent->currentState.u.player.movementDir;
        v26 = (float)cent->nextState.lerp.u.player.movementDir;
        v9 = v26 - movementDir;
        v27 = v9 * 0.002777777845039964;
        v8 = v27 + 0.5;
        v7 = floor(v8);
        v6 = (v27 - v7) * 360.0;
        ci->lerpMoveDir = v6 * f + movementDir;
        playerAngles = ci->playerAngles;
        ci->playerAngles[0] = cent->pose.angles[0];
        playerAngles[1] = cent->pose.angles[1];
        playerAngles[2] = cent->pose.angles[2];
        cent->pose.angles[0] = 0.0;
        cent->pose.angles[2] = 0.0;
        v22 = cent->currentState.u.player.leanf;
        v5 = cent->nextState.lerp.u.player.leanf - v22;
        v23 = v5 * 0.002777777845039964;
        v4 = v23 + 0.5;
        v3 = floor(v4);
        v2 = (v23 - v3) * 360.0;
        ci->lerpLean = v2 * f + v22;
    }
}

void __cdecl CG_CalcEntityPhysicsPositions(int localClientNum, centity_s *cent)
{
    cgs_t *cgs;

    iassert(cent);
    iassert(cent->currentState.pos.trType == TR_PHYSICS && cent->currentState.apos.trType == TR_PHYSICS);

    if (cent->nextValid && (cent->nextState.lerp.eFlags & 0x20) == 0 && cent->nextState.solid != 0xFFFFFF)
    {
        cgs = CG_GetLocalClientStaticGlobals(localClientNum);
        
        if (CG_PreProcess_GetDObj(
            localClientNum,
            cent->nextState.number,
            cent->nextState.eType,
            cgs->gameModels[cent->nextState.index.brushmodel])
            && !CG_ExpiredLaunch(localClientNum, cent))
        {
            if (!cent->pose.physObjId)
                CG_CreatePhysicsObject(localClientNum, cent);
            if (cent->pose.physObjId == -1)
            {
                cent->pose.origin[0] = cent->currentState.pos.trBase[0];
                cent->pose.origin[1] = cent->currentState.pos.trBase[1];
                cent->pose.origin[2] = cent->currentState.pos.trBase[2];
                cent->pose.angles[0] = cent->currentState.apos.trBase[0];
                cent->pose.angles[1] = cent->currentState.apos.trBase[1];
                cent->pose.angles[2] = cent->currentState.apos.trBase[2];
            }
            else
            {
                CG_UpdatePhysicsPose(cent);
            }
        }
    }
}

void __cdecl CG_CreatePhysicsObject(int localClientNum, centity_s *cent)
{
    const char *v2; // eax
    const char *Name; // eax
    float velocity[3]; // [esp+1Ch] [ebp-44h] BYREF
    dxBody *physObjId;
    DObj_s *obj; // [esp+2Ch] [ebp-34h]
    PhysPreset *physPreset; // [esp+30h] [ebp-30h]
    float quat[4]; // [esp+34h] [ebp-2Ch] BYREF
    float speed; // [esp+44h] [ebp-1Ch]
    float direction[3]; // [esp+48h] [ebp-18h] BYREF
    float position[3]; // [esp+54h] [ebp-Ch] BYREF

    velocity[0] = 0.0;
    velocity[1] = 0.0;
    velocity[2] = 0.0;
    position[0] = cent->currentState.pos.trBase[0];
    position[1] = cent->currentState.pos.trBase[1];
    position[2] = cent->currentState.pos.trBase[2];
    AnglesToQuat(cent->currentState.apos.trBase, quat);
    obj = Com_GetClientDObj(cent->nextState.number, localClientNum);
    iassert(obj);
    physPreset = DObjGetPhysPreset(obj);
    if (physPreset)
    {
        Sys_EnterCriticalSection(CRITSECT_PHYSICS);
        physObjId = Phys_ObjCreate(PHYS_WORLD_FX, position, quat, velocity, physPreset);
        if (physObjId)
        {
            DObjPhysicsSetCollisionFromXModel(obj, PHYS_WORLD_FX, physObjId);
            direction[0] = cent->currentState.apos.trDelta[0];
            direction[1] = cent->currentState.apos.trDelta[1];
            direction[2] = cent->currentState.apos.trDelta[2];
            speed = Vec3Normalize(direction);
            Phys_ObjBulletImpact(
                PHYS_WORLD_FX,
                physObjId,
                cent->currentState.pos.trDelta,
                direction,
                speed,
                physPreset->bulletForceScale);
            Sys_LeaveCriticalSection(CRITSECT_PHYSICS);
            cent->pose.physObjId = (intptr_t)physObjId;
        }
        else
        {
            Name = DObjGetName(obj);
            Com_PrintWarning(CON_CHANNEL_ERROR, "Failed to create physics object for '%s'.\n", Name);
            cent->pose.physObjId = -1;
            Sys_LeaveCriticalSection(CRITSECT_PHYSICS);
        }
    }
    else
    {
        cent->pose.physObjId = -1;
        v2 = DObjGetName(obj);
        Com_PrintWarning(CON_CHANNEL_ERROR, "Failed to create physics object for '%s'.  No physics preset.\n", v2);
    }
}

void __cdecl CG_UpdatePhysicsPose(centity_s *cent)
{
    float quat[4]; // [esp+0h] [ebp-10h] BYREF

    if (!cent->pose.physObjId || cent->pose.physObjId == -1)
        MyAssertHandler(
            ".\\cgame_mp\\cg_ents_mp.cpp",
            1281,
            0,
            "%s",
            "cent->pose.physObjId != PHYS_OBJ_ID_NULL && cent->pose.physObjId != PHYS_OBJ_ID_DEAD");
    Sys_EnterCriticalSection(CRITSECT_PHYSICS);
    Phys_ObjGetInterpolatedState(PHYS_WORLD_FX, (dxBody *)cent->pose.physObjId, cent->pose.origin, quat);
    Sys_LeaveCriticalSection(CRITSECT_PHYSICS);
    UnitQuatToAngles(quat, cent->pose.angles);
}

char __cdecl CG_ExpiredLaunch(int localClientNum, centity_s *cent)
{
    iassert(cent->nextValid);

    cg_s *cgameGlob = CG_GetLocalClientGlobals(localClientNum);

    if (cent->pose.physObjId || cgameGlob->time <= cent->nextState.lerp.pos.trTime + 1000)
        return 0;
    cent->pose.physObjId = -1;
    return 1;
}

void __cdecl CG_CalcEntityRagdollPositions(int localClientNum, centity_s *cent)
{
    iassert(cent);
    iassert(CG_IsRagdollTrajectory( &cent->currentState.pos ) || CG_IsRagdollTrajectory( &cent->currentState.apos ));
    if (!cent->pose.ragdollHandle && !cent->pose.killcamRagdollHandle)
        CG_CreateRagdollObject(localClientNum, cent);
    if (cent->pose.ragdollHandle || cent->pose.killcamRagdollHandle)
        CG_UpdateRagdollPose(cent);
}

void __cdecl CG_CreateRagdollObject(int localClientNum, centity_s *cent)
{
    int RagdollForDObj; // eax
    bool shareRagdoll; // [esp+Ah] [ebp-2h]
    bool reset; // [esp+Bh] [ebp-1h]
    const cg_s *cgameGlob;

    cgameGlob = CG_GetLocalClientGlobals(localClientNum);

    if (cgameGlob->inKillCam)
        reset = 0;
    else
        reset = (cent->nextState.lerp.eFlags & 0x80000) != 0;
    if (cent->nextState.eType == ET_PLAYER_CORPSE && cent->nextState.clientNum == cgameGlob->clientNum && cgameGlob->inKillCam)
    {
        shareRagdoll = 0;
        RagdollForDObj = Ragdoll_CreateRagdollForDObj(localClientNum, 0, cent->nextState.number, reset, 0);
    }
    else
    {
        shareRagdoll = 1;
        RagdollForDObj = Ragdoll_CreateRagdollForDObj(localClientNum, 0, cent->nextState.number, reset, 1);
    }
    if (shareRagdoll)
        cent->pose.ragdollHandle = RagdollForDObj;
    else
        cent->pose.killcamRagdollHandle = RagdollForDObj;
    cent->pose.isRagdoll = 1;
}

void __cdecl CG_UpdateRagdollPose(centity_s *cent)
{
    if (!cent->pose.ragdollHandle && !cent->pose.killcamRagdollHandle)
        MyAssertHandler(
            ".\\cgame_mp\\cg_ents_mp.cpp",
            1389,
            0,
            "%s",
            "cent->pose.ragdollHandle != RAGDOLL_INVALID || cent->pose.killcamRagdollHandle != RAGDOLL_INVALID");
    if (cent->pose.killcamRagdollHandle)
        Ragdoll_GetRootOrigin(cent->pose.killcamRagdollHandle, cent->pose.origin);
    else
        Ragdoll_GetRootOrigin(cent->pose.ragdollHandle, cent->pose.origin);
}

DObj_s *__cdecl CG_PreProcess_GetDObj(int localClientNum, int entIndex, int entType, XModel *model)
{
    XAnimTree_s *Tree; // [esp+18h] [ebp-20h]
    float *v6; // [esp+1Ch] [ebp-1Ch]
    DObjModel_s dobjModel; // [esp+20h] [ebp-18h] BYREF
    DObj_s *obj; // [esp+28h] [ebp-10h]
    XAnimTree_s *animTree; // [esp+2Ch] [ebp-Ch]
    centity_s *cent; // [esp+30h] [ebp-8h]
    XAnim_s *anims; // [esp+34h] [ebp-4h]

    obj = Com_GetClientDObj(entIndex, localClientNum);
    cent = CG_GetEntity(localClientNum, entIndex);
    if (obj && (!model || !CG_CheckDObjInfoMatches(localClientNum, entIndex, entType, model)))
    {
        if (cent->pose.physObjId != -1 && cent->pose.physObjId)
        {
            if (CG_IsEntityLinked(localClientNum, cent->nextState.number))
                CG_UnlinkEntity(localClientNum, cent->nextState.number);
            Phys_ObjDestroy(PHYS_WORLD_FX, (dxBody *)cent->pose.physObjId);
            cent->pose.physObjId = 0;
        }
        FX_MarkEntDetachAll(localClientNum, entIndex);
        CG_SafeDObjFree(localClientNum, entIndex);
        obj = 0;
    }
    if (!obj && model)
    {
        iassert(!cent->tree);
        anims = CG_GetAnimations(localClientNum, entIndex, entType);
        if (anims)
            Tree = XAnimCreateTree(anims, (void *(__cdecl *)(int))CG_AllocAnimTree);
        else
            Tree = 0;
        animTree = Tree;
        cent->tree = Tree;
        dobjModel.model = model;
        dobjModel.boneName = 0;
        dobjModel.ignoreCollision = 1;
        obj = Com_ClientDObjCreate(&dobjModel, 1u, animTree, entIndex, localClientNum);
        CG_SetDObjInfo(localClientNum, entIndex, entType, model);
        v6 = cg_entityOriginArray[localClientNum][cent->nextState.number];
        *v6 = 131072.0f;
        v6[1] = 131072.0f;
        v6[2] = 131072.0f;
        if (entType == ET_HELICOPTER)
            XAnimSetCompleteGoalWeight(obj, 1u, 1.0f, 0.2f, 1.5f, 0, 0, 1);
    }
    return obj;
}

XAnim_s *__cdecl CG_GetAnimations(int localClientNum, uint entIndex, int entType)
{
    centity_s *cent; // [esp+4h] [ebp-4h]
    centity_s *centa; // [esp+4h] [ebp-4h]

    if (entType == ET_MG42)
    {
        cent = CG_GetEntity(localClientNum, entIndex);
        return CG_GetMG42Anims(cent);
    }
    else if (entType == ET_HELICOPTER)
    {
        centa = CG_GetEntity(localClientNum, entIndex);
        return CG_GetHelicopterAnims(centa);
    }
    else
    {
        return 0;
    }
}

XAnim_s *__cdecl CG_GetMG42Anims(centity_s *cent)
{
    WeaponDef *weapDef; // [esp+0h] [ebp-8h]
    XAnim_s *pAnims; // [esp+4h] [ebp-4h]

    iassert(cent->nextState.weapon);
    weapDef = BG_GetWeaponDef(cent->nextState.weapon);
    pAnims = XAnimCreateAnims("MG42", 3u, (void *(__cdecl *)(int))Hunk_AllocXAnimClient);
    iassert(pAnims);
    XAnimBlend(pAnims, 0, "root", 1u, 2u, 0);
    BG_CreateXAnim(pAnims, 1u, (char *)weapDef->szXAnims[WEAP_ANIM_IDLE]);
    BG_CreateXAnim(pAnims, 2u, (char *)weapDef->szXAnims[WEAP_ANIM_FIRE]);
    return pAnims;
}

XAnim_s *__cdecl CG_GetHelicopterAnims(centity_s *cent)
{
    XAnim_s *pAnims; // [esp+4h] [ebp-4h]

    iassert(cent->nextState.weapon);
    BG_GetWeaponDef(cent->nextState.weapon);
    pAnims = XAnimCreateAnims("helicopter", 2u, (void *(__cdecl *)(int))Hunk_AllocXAnimClient);
    iassert(pAnims);
    XAnimBlend(pAnims, 0, "root", 1u, 1u, 0);
    BG_CreateXAnim(pAnims, 1u, "bh_rotors");
    return pAnims;
}

char *__cdecl CG_AllocAnimTree(int size)
{
    return (char*)MT_Alloc(size, MT_TYPE_SMALL_ANIM_TREE);
}

void __cdecl CG_DObjCalcBone(const cpose_t *pose, DObj_s *obj, int boneIndex)
{
    int partBits[4]; // [esp+0h] [ebp-10h] BYREF

    iassert(obj);
    iassert(pose);

    DObjLock(obj);

    if (CL_DObjCreateSkelForBone(obj, boneIndex))
    {
        DObjUnlock(obj);
    }
    else
    {
        DObjGetHierarchyBits(obj, boneIndex, partBits);
        CG_DoControllers(pose, obj, partBits);
        DObjCalcSkel(obj, partBits);
        DObjUnlock(obj);
    }
}

void __cdecl CG_ClearUnion(int localClientNum, centity_s *cent)
{
    switch (cent->pose.eTypeUnion)
    {
    case ET_PLAYER:
        *(_QWORD *)&cent->pose.player.control = 0;
        cent->pose.turret.barrelPitch = 0.0;
        break;
    case ET_FX:
    case ET_LOOP_FX:
        if (cent->pose.fx.effect)
            FX_ThroughWithEffect(localClientNum, cent->pose.fx.effect);
        *(_QWORD *)&cent->pose.player.control = 0;
        break;
    case ET_MG42:
        *(_QWORD *)&cent->pose.player.control = 0;
        *((_QWORD *)&cent->pose.fx + 1) = 0;
        break;
    case ET_HELICOPTER:
    case ET_VEHICLE:
        *(_QWORD *)&cent->pose.player.control = 0;
        *((_QWORD *)&cent->pose.fx + 1) = 0;
        *((_QWORD *)&cent->pose.fx + 2) = 0;
        *((_QWORD *)&cent->pose.fx + 3) = 0;
        *((uint *)&cent->pose.fx + 8) = 0;
        break;
    default:
        break;
    }
    cent->pose.eTypeUnion = ET_GENERAL;
}

void __cdecl CG_SetUnionType(int localClientNum, centity_s *cent)
{
    switch (cent->nextState.eType)
    {
    case ET_PLAYER:
    case ET_FX:
    case ET_LOOP_FX:
    case ET_MG42:
    case ET_HELICOPTER:
    case ET_VEHICLE:
        cent->pose.eTypeUnion = cent->nextState.eType;
        iassert(cent->pose.eTypeUnion == cent->nextState.eType);
        break;
    default:
        cent->pose.eTypeUnion = ET_GENERAL;
        break;
    }
}

void __cdecl CG_UpdatePoseUnion(int localClientNum, centity_s *cent)
{
    CG_ClearUnion(localClientNum, cent);
    CG_SetUnionType(localClientNum, cent);
}

void __cdecl CG_ProcessEntity(int localClientNum, centity_s *cent)
{
    CG_EntityEffects(localClientNum, cent);
    if (cent->nextState.eType != cent->pose.eTypeUnion)
        CG_UpdatePoseUnion(localClientNum, cent);
    switch (cent->nextState.eType)
    {
    case ET_GENERAL:
        CG_General(localClientNum, cent);
        break;
    case ET_PLAYER:
        CG_Player(localClientNum, cent);
        break;
    case ET_PLAYER_CORPSE:
        CG_Corpse(localClientNum, cent);
        break;
    case ET_ITEM:
        CG_Item(localClientNum, cent);
        break;
    case ET_MISSILE:
        CG_Missile(localClientNum, cent);
        break;
    case ET_INVISIBLE:
        return;
    case ET_SCRIPTMOVER:
        goto $LN6_8;
    case ET_SOUND_BLEND:
        CG_SoundBlend(localClientNum, cent);
        break;
    case ET_FX:
        CG_Fx(localClientNum, cent);
        break;
    case ET_LOOP_FX:
        CG_LoopFx(localClientNum, cent);
        break;
    case ET_PRIMARY_LIGHT:
        CG_PrimaryLight(localClientNum, cent);
        break;
    case ET_MG42:
        CG_mg42(localClientNum, cent);
        break;
    case ET_HELICOPTER:
    case ET_VEHICLE:
        CG_VehProcessEntity(localClientNum, cent);
        CG_CompassUpdateVehicleInfo(localClientNum, cent->nextState.number);
        break;
    case ET_PLANE:
        CG_CompassUpdateVehicleInfo(localClientNum, cent->nextState.number);
    $LN6_8:
        CG_ScriptMover(localClientNum, cent);
        break;
    default:
        Com_Error(ERR_DROP, "Bad entity type: %i", cent->nextState.eType);
        break;
    }
}

/*
==================
CG_General
==================
*/
void __cdecl CG_General(int localClientNum, centity_s *cent)
{
    DObj_s *obj; // [esp+4h] [ebp-18h]
    float lightingOrigin[3]; // [esp+10h] [ebp-Ch] BYREF
    cgs_t *cgs;

    // if set to invisible, skip
    if ((cent->nextState.lerp.eFlags & 0x20) == 0)
    {
        cgs = CG_GetLocalClientStaticGlobals(localClientNum);

        obj = CG_PreProcess_GetDObj(
            localClientNum,
            cent->nextState.number,
            cent->nextState.eType,
            cgs->gameModels[cent->nextState.index.brushmodel]);
        if (obj)
        {
            CG_LockLightingOrigin(cent, lightingOrigin);
            // add to refresh list
            R_AddDObjToScene(obj, &cent->pose, cent->nextState.number, 0, lightingOrigin, 0.0);
        }
    }
}

void __cdecl CG_LockLightingOrigin(centity_s *cent, float *lightingOrigin)
{
    if ((cent->nextState.lerp.eFlags & 0x400) != 0)
    {
        if (0.0 == cent->lightingOrigin[0] && 0.0 == cent->lightingOrigin[1] && 0.0 == cent->lightingOrigin[2])
        {
            cent->lightingOrigin[0] = cent->pose.origin[0];
            cent->lightingOrigin[1] = cent->pose.origin[1];
            cent->lightingOrigin[2] = cent->pose.origin[2];
        }
        *lightingOrigin = cent->lightingOrigin[0];
        lightingOrigin[1] = cent->lightingOrigin[1];
        lightingOrigin[2] = cent->lightingOrigin[2];
    }
    cent->lightingOrigin[0] = 0.0;
    cent->lightingOrigin[1] = 0.0;
    cent->lightingOrigin[2] = 0.0;
    *lightingOrigin = cent->pose.origin[0];
    lightingOrigin[1] = cent->pose.origin[1];
    lightingOrigin[2] = cent->pose.origin[2];
    lightingOrigin[2] = lightingOrigin[2] + 4.0;
}

/*
==================
CG_Item
==================
*/
void __cdecl CG_Item(int localClientNum, centity_s *cent)
{
    DObj_s *obj; // [esp+Ch] [ebp-20h]
    uint8_t weapModel; // [esp+17h] [ebp-15h]
    int weapIdx; // [esp+18h] [ebp-14h]
    float lightingOrigin[3]; // [esp+1Ch] [ebp-10h] BYREF
    WeaponDef *weapDef; // [esp+28h] [ebp-4h]

    if (cent->nextState.index.brushmodel >= 2048)
        Com_Error(ERR_DROP, "Bad item index %i on entity", cent->nextState.index.brushmodel);
    // if set to invisible, skip
    if ((cent->nextState.lerp.eFlags & 0x20) == 0)
    {
        weapIdx = cent->nextState.index.brushmodel % 128;
        weapModel = cent->nextState.index.brushmodel / 128;
        weapDef = BG_GetWeaponDef(weapIdx);
        iassert(weapDef);
        if (!weapDef->worldModel[weapModel])
            Com_Error(ERR_DROP, "No XModel loaded for item index %i, weap index %i, model %i (%s)", cent->nextState.index.brushmodel, weapIdx, weapModel, weapDef->szDisplayName);
        obj = CG_PreProcess_GetDObj(
            localClientNum,
            cent->nextState.number,
            cent->nextState.eType,
            weapDef->worldModel[weapModel]);
        if (obj)
        {
            lightingOrigin[0] = cent->pose.origin[0];
            lightingOrigin[1] = cent->pose.origin[1];
            lightingOrigin[2] = cent->pose.origin[2] + 4.0;
            // add to refresh list
            R_AddDObjToScene(obj, &cent->pose, cent->nextState.number, 0, lightingOrigin, 0.0);
        }
    }
}

/*
==================
CG_EntityEffects

Add continuous entity effects, like local entity emission and lighting
==================
*/
void __cdecl CG_EntityEffects(int localClientNum, centity_s *cent)
{
    if (cent->nextState.loopSound)
        CG_AddEntityLoopSound(localClientNum, cent);
    if (CG_IsRagdollTrajectory(&cent->currentState.pos))
        CG_CalcEntityLerpPositions(localClientNum, cent);
}

void __cdecl CG_AddEntityLoopSound(int localClientNum, const centity_s *cent)
{
    const char *ConfigString; // eax
    float midpoint[3]; // [esp+0h] [ebp-1Ch] BYREF
    float origin[3]; // [esp+Ch] [ebp-10h] BYREF
    GfxBrushModel *bmodel; // [esp+18h] [ebp-4h]

    if (cent->nextState.solid == 0xFFFFFF)
    {
        bmodel = R_GetBrushModel(cent->nextState.index.brushmodel);
        Vec3Avg(bmodel->bounds[0], bmodel->bounds[1], midpoint);
        Vec3Add(cent->pose.origin, midpoint, origin);
        ConfigString = CL_GetConfigString(localClientNum, cent->nextState.loopSound + 1342);
        CG_PlaySoundAliasByName(localClientNum, cent->nextState.number, origin, ConfigString);
    }
    else
    {
        CG_PlaySoundAliasByName(localClientNum,
            cent->nextState.number, 
            cent->pose.origin, 
            CL_GetConfigString(localClientNum, cent->nextState.loopSound + 1342)
        );
    }
}

void __cdecl CG_mg42(int localClientNum, centity_s *cent)
{
    DObj_s *obj; // [esp+Ch] [ebp-18h]
    float lightingOrigin[3]; // [esp+14h] [ebp-10h] BYREF
    const entityState_s *ns; // [esp+20h] [ebp-4h]
    const cg_s *cgameGlob;
    const cgs_t *cgs;

    cgs = CG_GetLocalClientStaticGlobals(localClientNum);
    cgameGlob = CG_GetLocalClientGlobals(localClientNum);

    ns = &cent->nextState;

    if ((cent->nextState.lerp.eFlags & 0x20) == 0)
    {
        obj = CG_PreProcess_GetDObj(localClientNum, ns->number, ns->eType, cgs->gameModels[ns->index.brushmodel]);
        if (obj)
        {
            CG_mg42_PreControllers(obj, cent);

            if (!CG_PlayerUsingScopedTurret(localClientNum))
                goto LABEL_12;

            if (cgameGlob->renderingThirdPerson)
                goto LABEL_12;

            if (cgameGlob->predictedPlayerState.viewlocked_entNum != ns->number)
            {
            LABEL_12:
                lightingOrigin[0] = cent->pose.origin[0];
                lightingOrigin[1] = cent->pose.origin[1];
                lightingOrigin[2] = cent->pose.origin[2] + 32.0f;
                R_AddDObjToScene(obj, &cent->pose, ns->number, 4, lightingOrigin, 0.0f);
            }
        }
    }
}

/*
===============
CG_Missile
===============
*/
void __cdecl CG_Missile(int localClientNum, centity_s *cent)
{
    DObj_s *obj; // [esp+10h] [ebp-20h]
    entityState_s *s1; // [esp+14h] [ebp-1Ch]
    float lightingOrigin[3]; // [esp+20h] [ebp-10h] BYREF
    WeaponDef *weapDef; // [esp+2Ch] [ebp-4h]

    s1 = &cent->nextState;
    if ((cent->nextState.lerp.eFlags & 0x20) == 0)
    {
        if (cent->nextState.lerp.u.missile.launchTime <= CG_GetLocalClientGlobals(localClientNum)->time)
        {
            if (cent->nextState.weapon >= BG_GetNumWeapons())
                cent->nextState.weapon = 0;
            vassert((localClientNum == 0), "(localClientNum) = %i", localClientNum);
            weapDef = BG_GetWeaponDef(cent->nextState.weapon);
            // add missile sound
            if (weapDef->projectileSound)
                CG_PlaySoundAlias(localClientNum, cent->nextState.number, cent->pose.origin, weapDef->projectileSound);
            // create the render entity
            obj = CG_PreProcess_GetDObj(
                localClientNum,
                cent->nextState.number,
                cent->nextState.eType,
                weapDef->projectileModel);
            if (obj)
            {
                // add trails
                if (weapDef->projTrailEffect && !cent->bTrailMade)
                {
                    cent->bTrailMade = 1;
                    CG_PlayBoltedEffect(localClientNum, weapDef->projTrailEffect, s1->number, scr_const.tag_fx);
                }
                lightingOrigin[0] = cent->pose.origin[0];
                lightingOrigin[1] = cent->pose.origin[1];
                lightingOrigin[2] = cent->pose.origin[2];
                lightingOrigin[2] = lightingOrigin[2] + 4.0;
                // add to refresh list, possibly with quad glow
                R_AddDObjToScene(obj, &cent->pose, s1->number, 0, lightingOrigin, 0.0);
                CG_AddHudGrenade(CG_GetLocalClientGlobals(localClientNum), cent);
            }
        }
    }
}

void __cdecl CG_ScriptMover(int localClientNum, centity_s *cent)
{
    const GfxBrushModel *BrushModel; // eax
    uint materialTime; // [esp+0h] [ebp-1Ch]
    DObj_s *obj; // [esp+4h] [ebp-18h]
    entityState_s *s1; // [esp+8h] [ebp-14h]
    float lightingOrigin[3]; // [esp+10h] [ebp-Ch] BYREF
    cgs_t *cgs;

    s1 = &cent->nextState;
    if ((cent->nextState.lerp.eFlags & 0x20) == 0)
    {
        cgs = CG_GetLocalClientStaticGlobals(localClientNum);
        
        if (cent->nextState.solid == 0xFFFFFF)
        {
            if (cent->nextValid && (cent->nextState.lerp.eFlags & 0x800) != 0)
                AimTarget_ProcessEntity(localClientNum, cent);
            materialTime = s1->number;
            BrushModel = R_GetBrushModel(cent->nextState.index.brushmodel);
            R_AddBrushModelToSceneFromAngles(BrushModel, cent->pose.origin, cent->pose.angles, materialTime);
        }
        else
        {
            obj = CG_PreProcess_GetDObj(
                localClientNum,
                cent->nextState.number,
                cent->nextState.eType,
                cgs->gameModels[cent->nextState.index.brushmodel]);
            if (obj)
            {
                CG_LockLightingOrigin(cent, lightingOrigin);
                R_AddDObjToScene(obj, &cent->pose, s1->number, 0, lightingOrigin, 0.0);
            }
        }
    }
}

void __cdecl CG_SoundBlend(int localClientNum, centity_s *cent)
{
    const char *ConfigString; // eax
    float lerp; // [esp+24h] [ebp-18h]
    snd_alias_t *alias1; // [esp+2Ch] [ebp-10h]
    snd_alias_t *alias0; // [esp+30h] [ebp-Ch]

    if (cent->nextState.eventParms[0])
    {
        if (cent->nextState.eventParms[1])
        {
            CL_GetConfigString(localClientNum, cent->nextState.eventParms[0] + 1342);
            if (CG_ShouldPlaySoundOnLocalClient())
            {
                ConfigString = CL_GetConfigString(localClientNum, cent->nextState.eventParms[0] + 1342);
                alias0 = CL_PickSoundAlias(ConfigString);
                alias1 = CL_PickSoundAlias(CL_GetConfigString(localClientNum, cent->nextState.eventParms[1] + 1342));
                if (alias0)
                {
                    if (alias1)
                    {
                        lerp = (cent->nextState.lerp.u.soundBlend.lerp - cent->currentState.u.soundBlend.lerp) * CG_GetLocalClientGlobals(localClientNum)->frameInterpolation
                            + cent->currentState.u.soundBlend.lerp;
                        iassert((lerp >= 0.0f) && (lerp <= 1.0f));
                        SND_PlayBlendedSoundAliases(
                            alias0,
                            alias1,
                            lerp,
                            1.0,
                            (SndEntHandle)cent->nextState.number,
                            cent->pose.origin,
                            0,
                            SASYS_CGAME);
                    }
                }
            }
        }
    }
}

void __cdecl CG_Fx(int localClientNum, centity_s *cent)
{
    if (cent->pose.fx.triggerTime != cent->nextState.time2)
    {
        if (cent->pose.fx.effect)
        {
            FX_AssertAllocatedEffect(localClientNum, cent->pose.fx.effect);
            FX_ThroughWithEffect(localClientNum, cent->pose.fx.effect);
        }
        *(_QWORD *)&cent->pose.player.control = 0;
        cent->pose.fx.effect = CG_StartFx(localClientNum, cent, cent->nextState.time2);
        if (cent->pose.fx.effect)
            cent->pose.fx.triggerTime = cent->nextState.time2;
    }
}

FxEffect *__cdecl CG_StartFx(int localClientNum, centity_s *cent, int startAtTime)
{
    const FxEffectDef *fxDef; // [esp+0h] [ebp-30h]
    int fxId; // [esp+8h] [ebp-28h]
    float axis[3][3]; // [esp+Ch] [ebp-24h] BYREF
    cgs_t *cgs;

    AnglesToAxis(cent->nextState.lerp.apos.trBase, axis);
    fxId = cent->nextState.un1.scale;
    rangeassert(fxId, 1, 99);
    cgs = CG_GetLocalClientStaticGlobals(localClientNum);
    fxDef = cgs->fxs[fxId];
    iassert(fxDef);
    return FX_SpawnOrientedEffect(localClientNum, fxDef, startAtTime, cent->pose.origin, axis, ENTITYNUM_NONE);
}

void __cdecl CG_LoopFx(int localClientNum, centity_s *cent)
{
    double v2; // [esp+0h] [ebp-20h]
    float diff[3]; // [esp+8h] [ebp-18h] BYREF
    const cg_s *cgameGlob; // [esp+14h] [ebp-Ch]
    int period; // [esp+18h] [ebp-8h]
    float cullDist; // [esp+1Ch] [ebp-4h]

    cgameGlob = CG_GetLocalClientGlobals(localClientNum);
    cullDist = cent->nextState.lerp.u.loopFx.cullDist;
    if (cullDist == 0.0
        || (Vec3Sub(cent->pose.origin, cgameGlob->predictedPlayerState.origin, diff),
            v2 = cullDist * cullDist,
            Vec3LengthSq(diff) < v2))
    {
        period = cent->nextState.lerp.u.loopFx.period;
        iassert(period > 0);
        if (!cent->pose.fx.effect)
        {
            cent->pose.fx.effect = CG_StartFx(localClientNum, cent, cgameGlob->time);
            if (!cent->pose.fx.effect)
                return;
            cent->pose.fx.triggerTime = period + cgameGlob->time;
        }
        while (cgameGlob->time >= cent->pose.fx.triggerTime)
        {
            FX_RetriggerEffect(localClientNum, cent->pose.fx.effect, cent->pose.fx.triggerTime);
            cent->pose.fx.triggerTime += period;
        }
    }
}

void __cdecl CG_PrimaryLight(int localClientNum, centity_s *cent)
{
    GfxLight *light; // [esp+2Ch] [ebp-34h]
    float oldColor[4]; // [esp+30h] [ebp-30h] BYREF
    const ComPrimaryLight *refLight; // [esp+40h] [ebp-20h]
    float lightAngles[3]; // [esp+44h] [ebp-1Ch] BYREF
    float newColor[4]; // [esp+50h] [ebp-10h] BYREF
    cg_s *cgameGlob;

    iassert(cent->nextState.eType == ET_PRIMARY_LIGHT);
    iassert(cent->nextState.index.primaryLight != PRIMARY_LIGHT_NONE);
    iassert(comWorld.isInUse);

    if (cent->nextState.index.brushmodel >= comWorld.primaryLightCount)
    {
        iassert(comWorld.isInUse);
        bcassert(cent->nextState.index.primaryLight, Com_GetPrimaryLightCount());
    }

    cgameGlob = CG_GetLocalClientGlobals(localClientNum);
    light = &cgameGlob->refdef.primaryLights[cent->nextState.index.brushmodel];
    refLight = Com_GetPrimaryLight(cent->nextState.index.brushmodel);
    Byte4UnpackRgba(&cent->currentState.u.primaryLight.colorAndExp[0], oldColor);
    Byte4UnpackRgba(&cent->nextState.lerp.u.primaryLight.colorAndExp[0], newColor);
    Vec3Scale(oldColor, cent->currentState.u.primaryLight.intensity, oldColor);
    Vec3Scale(newColor, cent->nextState.lerp.u.primaryLight.intensity, newColor);
    Vec3Lerp(oldColor, newColor, cgameGlob->frameInterpolation, light->color);

    if (refLight->rotationLimit < 1.0)
    {
        BG_EvaluateTrajectory(&cent->nextState.lerp.apos, cgameGlob->time, lightAngles);
        AngleVectors(lightAngles, light->dir, 0, 0);
        light->dir[0] = -light->dir[0];
        light->dir[1] = -light->dir[1];
        light->dir[2] = -light->dir[2];

        if (refLight->rotationLimit > -1.0)
            CG_ClampPrimaryLightDir(light, refLight);
    }

    if (refLight->translationLimit > 0.0)
    {
        BG_EvaluateTrajectory(&cent->nextState.lerp.pos, cgameGlob->time, light->origin);
        CG_ClampPrimaryLightOrigin(light, refLight);
    }

    light->radius = (cent->nextState.lerp.u.primaryLight.radius - cent->currentState.u.primaryLight.radius)
        * cgameGlob->frameInterpolation
        + cent->currentState.u.primaryLight.radius;
    light->cosHalfFovOuter = (cent->nextState.lerp.u.primaryLight.cosHalfFovOuter
        - cent->currentState.u.primaryLight.cosHalfFovOuter)
        * cgameGlob->frameInterpolation
        + cent->currentState.u.primaryLight.cosHalfFovOuter;
    light->cosHalfFovInner = (cent->nextState.lerp.u.primaryLight.cosHalfFovInner
        - cent->currentState.u.primaryLight.cosHalfFovInner)
        * cgameGlob->frameInterpolation
        + cent->currentState.u.primaryLight.cosHalfFovInner;
    light->exponent = (int)((double)(cent->nextState.lerp.u.primaryLight.colorAndExp[3]
        - cent->currentState.u.primaryLight.colorAndExp[3])
        * cgameGlob->frameInterpolation)
        + cent->currentState.u.primaryLight.colorAndExp[3];
    if (light->cosHalfFovOuter <= 0.0
        || light->cosHalfFovInner <= (double)light->cosHalfFovOuter
        || light->cosHalfFovInner > 1.0)
    {
        MyAssertHandler(
            ".\\cgame_mp\\cg_ents_mp.cpp",
            1169,
            0,
            "%s\n\t%s",
            "0.0f < light->cosHalfFovOuter && light->cosHalfFovOuter < light->cosHalfFovInner && light->cosHalfFovInner <= 1.0f",
            va("%g, %g", light->cosHalfFovOuter, light->cosHalfFovInner));
    }
}

const ComPrimaryLight *__cdecl Com_GetPrimaryLight(uint primaryLightIndex)
{
    iassert(comWorld.isInUse);
    bcassert(primaryLightIndex, comWorld.primaryLightCount);
    return &comWorld.primaryLights[primaryLightIndex];
}

void __cdecl CG_ClampPrimaryLightOrigin(GfxLight *light, const ComPrimaryLight *refLight)
{
    float scale; // [esp+Ch] [ebp-20h]
    float v3; // [esp+10h] [ebp-1Ch]
    float v4; // [esp+14h] [ebp-18h]
    float deltaLenSq; // [esp+18h] [ebp-14h]
    float lightDelta[3]; // [esp+20h] [ebp-Ch] BYREF

    Vec3Sub(light->origin, refLight->origin, lightDelta);
    deltaLenSq = Vec3LengthSq(lightDelta);
    v4 = refLight->translationLimit * refLight->translationLimit;
    if (deltaLenSq >= (double)v4)
    {
        v3 = sqrt(deltaLenSq);
        scale = refLight->translationLimit / v3;
        Vec3Mad(refLight->origin, scale, lightDelta, light->origin);
    }
}

void __cdecl CG_ClampPrimaryLightDir(GfxLight *light, const ComPrimaryLight *refLight)
{
    const char *v2; // eax
    float scale; // [esp+14h] [ebp-40h]
    double v4; // [esp+18h] [ebp-3Ch]
    float v5; // [esp+20h] [ebp-34h]
    double rotationLimit; // [esp+24h] [ebp-30h]
    float v7; // [esp+2Ch] [ebp-28h]
    float v8; // [esp+30h] [ebp-24h]
    float v9; // [esp+3Ch] [ebp-18h]
    float cosTurnAngle; // [esp+40h] [ebp-14h]
    float perpendicular[4]; // [esp+44h] [ebp-10h] BYREF

    cosTurnAngle = Vec3Dot(light->dir, refLight->dir);
    if (refLight->rotationLimit > (double)cosTurnAngle)
    {
        scale = -cosTurnAngle;
        Vec3Mad(light->dir, scale, refLight->dir, perpendicular);
        v9 = (1.0 - refLight->rotationLimit * refLight->rotationLimit) / (1.0 - cosTurnAngle * cosTurnAngle);
        v7 = sqrt(v9);
        perpendicular[3] = v7;
        Vec3ScaleMad(refLight->rotationLimit, refLight->dir, v7, perpendicular, light->dir);
        if (!Vec3IsNormalized(light->dir))
        {
            v4 = Vec3Length(light->dir);
            v2 = va("(%g %g %g) len %g", light->dir[0], light->dir[1], light->dir[2], v4);
            MyAssertHandler(".\\cgame_mp\\cg_ents_mp.cpp", 1116, 0, "%s\n\t%s", "Vec3IsNormalized( light->dir )", v2);
        }
        rotationLimit = refLight->rotationLimit;
        v8 = Vec3Dot(light->dir, refLight->dir) - rotationLimit;
        v5 = I_fabs(v8);
        if (v5 > EQUAL_EPSILON)
            MyAssertHandler(
                ".\\cgame_mp\\cg_ents_mp.cpp",
                1117,
                0,
                "%s",
                "I_I_fabs( Vec3Dot( light->dir, refLight->dir ) - refLight->rotationLimit ) <= 0.001f");
    }
}

void __cdecl CG_GetPoseOrigin(const cpose_t *pose, float *origin)
{
    iassert(pose);
    *origin = pose->origin[0];
    origin[1] = pose->origin[1];
    origin[2] = pose->origin[2];
}

void __cdecl CG_GetPoseAngles(const cpose_t *pose, float *angles)
{
    iassert(pose);
    *angles = pose->angles[0];
    angles[1] = pose->angles[1];
    angles[2] = pose->angles[2];
}

float *__cdecl CG_GetEntityOrigin(int localClientNum, uint entnum)
{
    return CG_GetEntity(localClientNum, entnum)->pose.origin;
}

void __cdecl CG_PredictiveSkinCEntity(GfxSceneEntity *sceneEnt)
{
    cpose_t *pose; // [esp+0h] [ebp-4h]

    iassert(sceneEnt);
    pose = sceneEnt->info.pose;
    if (pose->cullIn == 1)
    {
        pose->cullIn = 0;
        R_UpdateXModelBoundsDelayed(sceneEnt);
    }
    else if (pose->cullIn == 2)
    {
        pose->cullIn = 0;
        R_SkinGfxEntityDelayed(sceneEnt);
    }
}


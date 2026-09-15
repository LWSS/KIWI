#ifndef KISAK_SP
#error This file is for SinglePlayer only
#endif

#include <universal/q_shared.h>
#include "cg_ents.h"
#include <qcommon/com_bsp.h>
#include <gfx_d3d/r_scene.h>
#include "cg_actors.h"
#include <xanim/dobj_utils.h>
#include <EffectsCore/fx_system.h>
#include <win32/win_local.h>
#include <physics/phys_local.h>
#include <game/savememory.h>
#include <ragdoll/ragdoll.h>
#include <gfx_d3d/r_workercmds_common.h>
#include "cg_main.h"
#include <script/scr_const.h>
#include "cg_local.h"
#include <gfx_d3d/r_model.h>
#include "cg_pose.h"
#include "cg_compassfriendlies.h"

#include <bgame/bg_local.h>

void __cdecl LocalConvertQuatToMat(const DObjAnimMat *mat, float (*axis)[3])
{
    double v4; // fp5
    double v5; // fp12
    double v6; // fp4
    double v7; // fp13
    double v8; // fp0
    double v9; // fp11
    double v10; // fp10
    double v11; // fp8
    double v12; // fp9

    iassert(!IS_NAN((mat->quat)[0]) && !IS_NAN((mat->quat)[1]) && !IS_NAN((mat->quat)[2]) && !IS_NAN((mat->quat)[3]));
    iassert(!IS_NAN(mat->transWeight));
    v4 = (float)(mat->quat[2] * (float)(mat->quat[0] * mat->transWeight));
    v5 = (float)(mat->quat[1] * (float)(mat->quat[0] * mat->transWeight));
    v6 = (float)(mat->quat[2] * (float)(mat->quat[1] * mat->transWeight));
    v7 = (float)(mat->quat[3] * (float)(mat->quat[0] * mat->transWeight));
    v8 = (float)(mat->quat[3] * (float)(mat->quat[1] * mat->transWeight));
    v9 = (float)(mat->quat[3] * (float)(mat->quat[2] * mat->transWeight));
    v10 = (float)((float)(mat->quat[2] * (float)(mat->quat[2] * mat->transWeight))
        + (float)(mat->quat[1] * (float)(mat->quat[1] * mat->transWeight)));
    v11 = (float)((float)(mat->quat[2] * (float)(mat->quat[2] * mat->transWeight))
        + (float)(mat->quat[0] * (float)(mat->quat[0] * mat->transWeight)));
    v12 = (float)((float)(mat->quat[1] * (float)(mat->quat[1] * mat->transWeight))
        + (float)(mat->quat[0] * (float)(mat->quat[0] * mat->transWeight)));
    (*axis)[5] = (float)(mat->quat[2] * (float)(mat->quat[1] * mat->transWeight))
        + (float)(mat->quat[3] * (float)(mat->quat[0] * mat->transWeight));
    (*axis)[2] = (float)v4 - (float)v8;
    (*axis)[6] = (float)v8 + (float)v4;
    (*axis)[3] = (float)v5 - (float)v9;
    (*axis)[0] = (float)1.0 - (float)v10;
    (*axis)[1] = (float)v9 + (float)v5;
    (*axis)[4] = (float)1.0 - (float)v11;
    (*axis)[7] = (float)v6 - (float)v7;
    (*axis)[8] = (float)1.0 - (float)v12;
}

const ComPrimaryLight *__cdecl Com_GetPrimaryLight(unsigned int primaryLightIndex)
{
    iassert(comWorld.isInUse);
    bcassert(primaryLightIndex, comWorld.primaryLightCount);
    return &comWorld.primaryLights[primaryLightIndex];
}

void __cdecl CG_LockLightingOrigin(centity_s *cent, float *lightingOrigin)
{
    if ((cent->nextState.lerp.eFlags & 0x400) != 0)
    {
        if (cent->lightingOrigin[0] == 0.0 && cent->lightingOrigin[1] == 0.0 && cent->lightingOrigin[2] == 0.0)
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
    lightingOrigin[2] = cent->pose.origin[2] + (float)4.0;
}

int __cdecl CG_GetRenderFlagForRefEntity(__int16 eFlags)
{
    unsigned int v1; // r11
    int v2; // r10
    int result; // r3
    bool v4; // zf

    v1 = eFlags & 0xE000;
    v2 = 0;
    if (v1 > 0x6000)
    {
        if (v1 == 0x8000)
        {
            v2 = 2048;
        }
        else if (v1 == 40960)
        {
            v2 = 256;
        }
    }
    else if ((eFlags & 0xE000) == 0x6000)
    {
        v2 = 1536;
    }
    else if (v1 == 0x2000)
    {
        v2 = 512;
    }
    else if (v1 == 0x4000)
    {
        v2 = 1024;
    }
    v4 = (eFlags & 0x80) == 0;
    result = v2 | 8;
    if (v4)
        return v2;
    return result;
}

/*
==================
CG_General
==================
*/
void __cdecl CG_General(int localClientNum, centity_s *cent)
{
    entityState_s *p_nextState; // r31
    DObj_s *obj;
    float lightingOrigin[3];

    p_nextState = &cent->nextState;
    // if set to invisible, skip
    if ((cent->nextState.lerp.eFlags & 0x20) == 0)
    {
        obj = Com_GetClientDObj(cent->nextState.number, 0);
        if (obj)
        {
            CG_LockLightingOrigin(cent, lightingOrigin);
            // add to refresh list
            R_AddDObjToScene(obj, &cent->pose, p_nextState->number, CG_GetRenderFlagForRefEntity(p_nextState->lerp.eFlags), lightingOrigin, 0.0f);
        }
    }
}

/*
==================
CG_Item
==================
*/
void __cdecl CG_Item(centity_s *cent)
{
    entityState_s *p_nextState; // r31
    int v3; // r10
    unsigned __int8 v4; // r28
    unsigned int v5; // r29
    WeaponDef *weapDef; // r30
    unsigned int RenderFlagForRefEntity; // r3
    const DObj_s *obj; // r9

    p_nextState = &cent->nextState;

    if (cent->nextState.index.item >= 0x800u)
        Com_Error(ERR_DROP, "Bad item index %i on entity", cent->nextState.index);

    // if set to invisible, skip
    if ((p_nextState->lerp.eFlags & 0x20) == 0)
    {
        v3 = p_nextState->index.item;
        v4 = (unsigned int)v3 >> 7;
        v5 = v3 - (v3 >> 7 << 7);
        weapDef = BG_GetWeaponDef(v5);
        iassert(weapDef);
        if (!weapDef->worldModel[v4])
            Com_Error(ERR_DROP, "Bad item/weap index"); //KISAKTODO: "XModel loaded for item index %i weap index %i model %i (%s)", *(unsigned __int16 *)p_nextState->index, v5);

        obj = Com_GetClientDObj(p_nextState->number, 0);

        if (obj)
        {
            float lightingOrigin[3];
            lightingOrigin[0] = cent->pose.origin[0];
            lightingOrigin[1] = cent->pose.origin[1];
            lightingOrigin[2] = cent->pose.origin[2];

            RenderFlagForRefEntity = CG_GetRenderFlagForRefEntity(p_nextState->lerp.eFlags);
            // add to refresh list
            R_AddDObjToScene(obj, &cent->pose, p_nextState->number, RenderFlagForRefEntity, lightingOrigin, 0.0f);
        }
    }
}

void __cdecl CG_AddEntityLoopSound(int localClientNum, const centity_s *cent)
{
    float *BrushModel; // r3
    double v5; // fp10
    unsigned int v6; // r4
    double v7; // fp9
    double v8; // fp12
    double v9; // fp0
    const char *ConfigString; // r3
    const float *origin; // r5
    float v12[6]; // [sp+50h] [-30h] BYREF

    if (cent->nextState.solid == 0xFFFFFF)
    {
        BrushModel = (float *)R_GetBrushModel(cent->nextState.index.item);
        v5 = cent->pose.origin[1];
        v6 = cent->nextState.loopSound + CS_SOUNDALIASES;
        v7 = cent->pose.origin[2];
        v8 = (float)((float)(BrushModel[7] + BrushModel[10]) * (float)0.5);
        v9 = (float)((float)(BrushModel[8] + BrushModel[11]) * (float)0.5);
        v12[0] = cent->pose.origin[0] + (float)((float)(BrushModel[9] + BrushModel[6]) * (float)0.5);
        v12[1] = (float)v5 + (float)v8;
        v12[2] = (float)v7 + (float)v9;
        ConfigString = CL_GetConfigString(localClientNum, v6);
        origin = v12;
    }
    else
    {
        ConfigString = CL_GetConfigString(localClientNum, cent->nextState.loopSound + CS_SOUNDALIASES);
        origin = cent->pose.origin;
    }
    CG_PlaySoundAliasByName(localClientNum, cent->nextState.number, origin, ConfigString);
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
}

void __cdecl CG_mg42_PreControllers(int localClientNum, const DObj_s *obj, centity_s *cent)
{
    bool v5; // r11

    vassert((localClientNum == 0), "(localClientNum) = %i", localClientNum);
    if ((cgArray[0].predictedPlayerState.eFlags & 0x300) == 0
        || (v5 = 1, cgArray[0].predictedPlayerState.viewlocked_entNum != cent->nextState.number))
    {
        v5 = 0;
    }
    cent->pose.turret.playerUsing = v5;
    if (v5)
    {
        cent->pose.cullIn = 0;
        cent->pose.turret.viewAngles = cgArray[0].refdefViewAngles;
    }
    else
    {
        cent->pose.turret.angles.pitch = LerpAngle(
            cent->currentState.u.turret.gunAngles[0],
            cent->nextState.lerp.u.turret.gunAngles[0],
            cgArray[0].frameInterpolation);
        cent->pose.actor.pitch = LerpAngle(
            cent->currentState.u.turret.gunAngles[1],
            cent->nextState.lerp.u.turret.gunAngles[1],
            cgArray[0].frameInterpolation);
        cent->pose.actor.roll = LerpAngle(
            cent->currentState.u.turret.gunAngles[2],
            cent->nextState.lerp.u.turret.gunAngles[2],
            cgArray[0].frameInterpolation);
    }
    DObjGetBoneIndex(obj, scr_const.tag_aim, &cent->pose.turret.tag_aim);
    DObjGetBoneIndex(obj, scr_const.tag_aim_animated, &cent->pose.turret.tag_aim_animated);
    DObjGetBoneIndex(obj, scr_const.tag_flash, &cent->pose.turret.tag_flash);
}

void __cdecl CG_mg42(int localClientNum, centity_s *cent)
{
    entityState_s *p_nextState; // r30
    const DObj_s *obj; // r3

    p_nextState = &cent->nextState;
    if ((cent->nextState.lerp.eFlags & 0x20) == 0)
    {
        obj = Com_GetClientDObj(cent->nextState.number, 0);
        if (obj)
        {
            CG_mg42_PreControllers(localClientNum, obj, cent);
            if (!CG_PlayerUsingScopedTurret(localClientNum)
                || CG_GetLocalClientGlobals(localClientNum)->predictedPlayerState.viewlocked_entNum != p_nextState->number)
            {
                // KISAKTODO: blops offsets this by +32 [Z], should we copy?
                float lightingOrigin[3];
                lightingOrigin[0] = cent->pose.origin[0];
                lightingOrigin[1] = cent->pose.origin[1];
                lightingOrigin[2] = cent->pose.origin[2];
                R_AddDObjToScene(obj, &cent->pose, p_nextState->number, CG_GetRenderFlagForRefEntity(p_nextState->lerp.eFlags) | 4, lightingOrigin, 0.0f);
            }
        }
    }
}

bool __cdecl JavelinSoftLaunch(WeaponDef *weapDef, cg_s *cgameGlob, entityState_s *s1)
{
    return weapDef->guidedMissileType == MISSILE_GUIDANCE_JAVELIN
        && cgameGlob->time - s1->lerp.u.missile.launchTime <= weapDef->projIgnitionDelay;
}

/*
===============
CG_Missile
===============
*/
void __cdecl CG_Missile(int localClientNum, centity_s *cent)
{
    WeaponDef *WeaponDef; // r27
    snd_alias_list_t *projectileSound; // r6
    const DObj_s *ClientDObj; // r25
    const FxEffectDef *projTrailEffect; // r4
    const FxEffectDef *projIgnitionEffect; // r4
    snd_alias_list_t *projIgnitionSound; // r6
    unsigned int RenderFlagForRefEntity; // r3

    vassert((localClientNum == 0), "(localClientNum) = %i", localClientNum);
    if ((cent->nextState.lerp.eFlags & 0x20) == 0
        && cent->nextState.lerp.u.missile.launchTime <= CG_GetLocalClientTime(localClientNum))
    {
        if (cent->nextState.weapon >= BG_GetNumWeapons())
            cent->nextState.weapon = 0;
        CG_GetLocalClientWeaponInfo(localClientNum, cent->nextState.weapon);
        WeaponDef = BG_GetWeaponDef(cent->nextState.weapon);
        projectileSound = WeaponDef->projectileSound;
        // add missile sound
        if (projectileSound)
            CG_PlaySoundAlias(localClientNum, cent->nextState.number, cent->pose.origin, projectileSound);
        ClientDObj = Com_GetClientDObj(cent->nextState.number, 0);
        // create the render entity
		if (!ClientDObj && WeaponDef->projectileModel)
		{
			DObjModel_s dobjModel;
			dobjModel.model = WeaponDef->projectileModel;
			dobjModel.boneName = 0;
			dobjModel.ignoreCollision = 1;
			ClientDObj = Com_ClientDObjCreate(&dobjModel, 1u, 0, cent->nextState.number, 0);
		}
        if (ClientDObj)
        {
            // add trails
            if (!cent->bTrailMade
                && (WeaponDef->guidedMissileType != MISSILE_GUIDANCE_JAVELIN
                    || cgArray[0].time - cent->nextState.lerp.u.missile.launchTime > WeaponDef->projIgnitionDelay))
            {
                cent->bTrailMade = 1;
                projTrailEffect = WeaponDef->projTrailEffect;
                if (projTrailEffect)
                    CG_PlayBoltedEffect(localClientNum, projTrailEffect, cent->nextState.number, scr_const.tag_fx);
                projIgnitionEffect = WeaponDef->projIgnitionEffect;
                if (projIgnitionEffect)
                    CG_PlayBoltedEffect(localClientNum, projIgnitionEffect, cent->nextState.number, scr_const.tag_fx);
                projIgnitionSound = WeaponDef->projIgnitionSound;
                if (projIgnitionSound)
                    CG_PlaySoundAlias(localClientNum, cent->nextState.number, cent->pose.origin, projIgnitionSound);
            }
            RenderFlagForRefEntity = CG_GetRenderFlagForRefEntity(cent->nextState.lerp.eFlags);
            //R_AddDObjToScene(ClientDObj, &cent->pose, cent->nextState.number, RenderFlagForRefEntity, v12, v13);
            // lwss add from blops
            float lightingOrigin[3];
            lightingOrigin[0] = cent->pose.origin[0];
            lightingOrigin[1] = cent->pose.origin[1];
            lightingOrigin[2] = cent->pose.origin[2];
            lightingOrigin[2] = lightingOrigin[2] + 4.0;
            // lwss end
            // add to refresh list, possibly with quad glow
            R_AddDObjToScene(ClientDObj, &cent->pose, cent->nextState.number, RenderFlagForRefEntity, lightingOrigin, 0.0f);
            CG_AddHudGrenade(cgArray, cent);
        }
    }
}

static float g_entMoveTolVec[3] = { 16.0, 16.0, 16.0 };

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

void __cdecl CG_UpdateBModelWorldBounds(unsigned int localClientNum, centity_s *cent, int forceFilter)
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

void __cdecl CG_ScriptMover(int localClientNum, centity_s *cent)
{
    entityState_s *p_nextState; // r31
    DObj_s *ClientDObj; // r29
    unsigned int RenderFlagForRefEntity; // r3
    cg_s *LocalClientGlobals; // r3
    unsigned int number; // r29
    const GfxBrushModel *BrushModel; // r3
    float lightingOrigin[3];

    p_nextState = &cent->nextState;
    if ((cent->nextState.lerp.eFlags & 0x20) == 0)
    {
        if (cent->nextState.solid == 0xFFFFFF)
        {
            number = cent->nextState.number;
            BrushModel = R_GetBrushModel(cent->nextState.index.item);
            R_AddBrushModelToSceneFromAngles(BrushModel, cent->pose.origin, cent->pose.angles, number);
        }
        else
        {
            ClientDObj = Com_GetClientDObj(cent->nextState.number, 0);
            if (ClientDObj)
            {
                if ((CG_LockLightingOrigin(cent, lightingOrigin),
                    RenderFlagForRefEntity = CG_GetRenderFlagForRefEntity(p_nextState->lerp.eFlags),
                    R_AddDObjToScene(ClientDObj, &cent->pose, p_nextState->number, RenderFlagForRefEntity, lightingOrigin, 0.0f),
                    CG_LookingThroughNightVision(localClientNum))
                    && (p_nextState->lerp.eFlags & 0x4000) != 0
                    || cg_laserForceOn->current.enabled)
                {
                    LocalClientGlobals = CG_GetLocalClientGlobals(localClientNum);
                    CG_Laser_Add(cent, ClientDObj, &cent->pose, LocalClientGlobals->refdef.viewOffset, LASER_OWNER_NON_PLAYER);
                }
            }
        }
    }
}

/*
=========================
CG_AdjustPositionForMover

Also called by client movement prediction code
=========================
*/
void __cdecl CG_AdjustPositionForMover(
    int localClientNum,
    float *in,
    int moverNum,
    int fromTime,
    int toTime,
    float *out,
    float *outDeltaAngles)
{
    centity_s *Entity; // r3
    centity_s *v13; // r27
    const trajectory_t *p_pos; // r28
    const trajectory_t *p_apos; // r27
    double v16; // fp13
    double v17; // fp12
    double v18; // fp11
    double v19; // fp10
    double v20; // fp7
    double v21; // fp9
    double v22; // fp9
    float v23[4]; // [sp+50h] [-80h] BYREF
    float v24[4]; // [sp+60h] [-70h] BYREF
    float v25[4]; // [sp+70h] [-60h] BYREF
    float v26[20]; // [sp+80h] [-50h] BYREF

    if (outDeltaAngles)
    {
        *outDeltaAngles = 0.0;
        outDeltaAngles[1] = 0.0;
        outDeltaAngles[2] = 0.0;
    }
    if ((unsigned int)(moverNum - 1) <= 0x87C
        && (Entity = CG_GetEntity(localClientNum, moverNum), v13 = Entity, Entity->nextState.eType == ET_SCRIPTMOVER))
    {
        p_pos = &Entity->currentState.pos;
        BG_EvaluateTrajectory(&Entity->currentState.pos, fromTime, v23);
        p_apos = &v13->currentState.apos;
        BG_EvaluateTrajectory(p_apos, fromTime, v25);
        BG_EvaluateTrajectory(p_pos, toTime, v24);
        BG_EvaluateTrajectory(p_apos, toTime, v26);
        v16 = (float)(v24[1] - v23[1]);
        v17 = (float)(v24[2] - v23[2]);
        v18 = (float)(v26[0] - v25[0]);
        v19 = (float)(v26[1] - v25[1]);
        v20 = v26[2];
        *out = *in + (float)(v24[0] - v23[0]);
        out[1] = in[1] + (float)v16;
        v21 = v25[2];
        out[2] = in[2] + (float)v17;
        v22 = (float)((float)v20 - (float)v21);
        if (outDeltaAngles)
        {
            *outDeltaAngles = v18;
            outDeltaAngles[1] = v19;
            outDeltaAngles[2] = v22;
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

void __cdecl CG_SetFrameInterpolation(int localClientNum)
{
    snapshot_s *nextSnap; // r11
    __int64 v2; // r11
    __int64 v3; // r10
    __int64 v4; // [sp+50h] [-30h]

    vassert((localClientNum == 0), "(localClientNum) = %i", localClientNum);
    if (!cgArray[0].snap)
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\cgame\\cg_ents.cpp", 487, 0, "%s", "cgameGlob->snap");
    nextSnap = cgArray[0].nextSnap;
    if (!cgArray[0].nextSnap)
    {
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\cgame\\cg_ents.cpp", 488, 0, "%s", "cgameGlob->nextSnap");
        nextSnap = cgArray[0].nextSnap;
    }
    int snapTime = cgArray[0].snap->serverTime;
    int snapDelta = nextSnap->serverTime - snapTime;
    if (snapDelta)
    {
        cgArray[0].frameInterpolation = (float)(cgArray[0].time - snapTime) / (float)snapDelta;
        if (cgArray[0].frameInterpolation < 0.0)
            cgArray[0].frameInterpolation = 0.0;
    }
    else
    {
        cgArray[0].frameInterpolation = 0.0;
    }
}

void __cdecl CG_DObjUpdateInfo(const cg_s *cgameGlob, DObj_s *obj, bool notify)
{
    DObjUpdateClientInfo(obj, (float)cgameGlob->animFrametime * 0.001f, obj != nullptr);
}

cpose_t *__cdecl CG_GetPose(int localClientNum, int handle)
{
    iassert((handle >= 0 && handle < (((MAX_GENTITIES)) + 128)));

    if (handle < MAX_GENTITIES)
        return &CG_GetEntity(localClientNum, handle)->pose;

    iassert((handle >= ((MAX_GENTITIES)) && handle - ((MAX_GENTITIES)) < 128));

    vassert((localClientNum == 0), "(localClientNum) = %i", localClientNum);
    return &cgArray[0].viewModelPose;
}

unsigned __int16 *g_wheelTags[6] =
{
  &scr_const.tag_wheel_front_left,
  &scr_const.tag_wheel_front_right,
  &scr_const.tag_wheel_back_left,
  &scr_const.tag_wheel_back_right,
  &scr_const.tag_wheel_middle_left,
  &scr_const.tag_wheel_middle_right,
};

void __cdecl CG_Vehicle_PreControllers(int localClientNum, const DObj_s *obj, centity_s *cent)
{
    float axis[4][3];
    float wheelOrigin[3];
    float traceStart[3];
    float traceEnd[3];
    trace_t trace;

    iassert(obj);

    cg_s *cgameGlob = CG_GetLocalClientGlobals(localClientNum);

    cent->pose.vehicle.pitch = (int)floor(LerpAngle(cent->currentState.u.vehicle.bodyPitch, cent->nextState.lerp.u.vehicle.bodyPitch, cgameGlob->frameInterpolation) * 182.04445f + 0.5f);
    cent->pose.vehicle.roll = (int)floor(LerpAngle(cent->currentState.u.vehicle.bodyRoll, cent->nextState.lerp.u.vehicle.bodyRoll, cgameGlob->frameInterpolation) * 182.04445f + 0.5f);
    cent->pose.vehicle.barrelPitch = (int)floor(LerpAngle(cent->currentState.u.vehicle.gunPitch, cent->nextState.lerp.u.vehicle.gunPitch, cgameGlob->frameInterpolation) * 182.04445f + 0.5f);
    cent->pose.vehicle.yaw = (int)floor(LerpAngle(cent->currentState.u.vehicle.gunYaw, cent->nextState.lerp.u.vehicle.gunYaw, cgameGlob->frameInterpolation) * 182.04445f + 0.5f);
    cent->pose.vehicle.steerYaw = (int)floor( LerpAngle(cent->currentState.u.vehicle.steerYaw, cent->nextState.lerp.u.vehicle.steerYaw, cgameGlob->frameInterpolation) * 182.04445f + 0.5f);

    cent->pose.vehicle.time = (float)cent->nextState.time2 * 0.001f;
    DObjGetBoneIndex(obj, scr_const.tag_body, &cent->pose.vehicle.tag_body);
    DObjGetBoneIndex(obj, scr_const.tag_turret, &cent->pose.vehicle.tag_turret);
    DObjGetBoneIndex(obj, scr_const.tag_barrel, &cent->pose.vehicle.tag_barrel);

    if (cent->pose.cullIn != 2)
    {
        memset(cent->pose.vehicle.wheelBoneIndex, 254, sizeof(cent->pose.vehicle.wheelBoneIndex));
        return;
    }

    AnglesToAxis(cent->pose.angles, axis);
    Vec3Copy(cent->pose.origin, axis[3]);

    const XModel *model = DObjGetModel(obj, 0);
    const DObjAnimMat *basePose = XModelGetBasePose(model);

    iassert(*g_wheelTags[0] == scr_const.tag_wheel_front_left);
    iassert(*g_wheelTags[1] == scr_const.tag_wheel_front_right);

    for (int wheelIndex = 0; wheelIndex < ARRAY_COUNT(g_wheelTags); ++wheelIndex)
    {
        unsigned __int8 *boneIndex = &cent->pose.vehicle.wheelBoneIndex[wheelIndex];
        if (DObjGetBoneIndex(obj, *g_wheelTags[wheelIndex], boneIndex))
        {
            MatrixTransformVector43(basePose[*boneIndex].trans, (const mat4x3 &)axis, wheelOrigin);
            Vec3Mad(wheelOrigin, 40.0f, axis[2], traceStart);
            Vec3Mad(wheelOrigin, -cent->pose.vehicle.time, axis[2], traceEnd);
            CG_TraceCapsule(&trace, traceStart, vec3_origin, vec3_origin, traceEnd, cent->nextState.number, CONTENTS_SOLID | CONTENTS_GLASS | CONTENTS_VEHICLECLIP);
            cent->pose.vehicle.wheelFraction[wheelIndex] = CompressUnit(trace.fraction);
        }
    }
}

void __cdecl CG_Vehicle(int localClientNum, centity_s *cent)
{
    entityState_s *p_nextState; // r30
    const DObj_s *obj; // r3
    int RenderFlagForRefEntity; // r3
    cg_s *cgameGlob;
    float lightingOrigin[3];
    float materialTime;
    int time;

    p_nextState = &cent->nextState;
    if ((cent->nextState.lerp.eFlags & 0x20) == 0)
    {
        obj = Com_GetClientDObj(cent->nextState.number, 0);
        if (obj)
        {
            CG_Vehicle_PreControllers(localClientNum, obj, cent);

            lightingOrigin[0] = cent->pose.origin[0];
            lightingOrigin[1] = cent->pose.origin[1];
            lightingOrigin[2] = cent->pose.origin[2] + 32.0f;
            cgameGlob = CG_GetLocalClientGlobals(localClientNum);
            if (!p_nextState->lerp.u.vehicle.materialTime && !cent->currentState.u.vehicle.materialTime)
            {
                materialTime = 0.0f;
            }
            else
            {
                time = cent->currentState.u.vehicle.materialTime
                    + (int)((float)(p_nextState->lerp.u.vehicle.materialTime - cent->currentState.u.vehicle.materialTime)
                        * cgameGlob->frameInterpolation);
                materialTime = (float)(cgameGlob->time - time) * 0.001f;
            }
            RenderFlagForRefEntity = CG_GetRenderFlagForRefEntity(p_nextState->lerp.eFlags);
            R_AddDObjToScene(obj, &cent->pose, p_nextState->number, RenderFlagForRefEntity | 4, lightingOrigin, materialTime);
            if (p_nextState->eType == ET_VEHICLE)
                CG_CompassUpdateVehicleInfo(localClientNum, p_nextState->number);
        }
    }
}

void __cdecl CG_SoundBlend(int localClientNum, centity_s *cent)
{
    entityState_s *p_nextState; // r31
    unsigned int v5; // r11
    const char *ConfigString; // r3
    const snd_alias_t *alias0; // r28
    const char *v8; // r3
    snd_alias_t *v9; // r3
    const snd_alias_t *alias1; // r27
    const float *v11; // r6
    int v12; // r5
    cg_s *LocalClientGlobals; // r3
    double lerp; // fp31
    double volumeScale; // fp30

    p_nextState = &cent->nextState;
    v5 = cent->nextState.eventParms[0];
    if (v5)
    {
        if (cent->nextState.eventParms[1])
        {
            ConfigString = CL_GetConfigString(localClientNum, v5 + CS_SOUNDALIASES);
            alias0 = CL_PickSoundAlias(ConfigString);
            v8 = CL_GetConfigString(localClientNum, p_nextState->eventParms[1] + CS_SOUNDALIASES);
            v9 = CL_PickSoundAlias(v8);
            alias1 = v9;
            if (alias0)
            {
                if (v9)
                {
                    LocalClientGlobals = CG_GetLocalClientGlobals(localClientNum);
                    lerp = (float)((float)((float)(p_nextState->lerp.u.turret.gunAngles[0]
                        - cent->currentState.u.turret.gunAngles[0])
                        * LocalClientGlobals->frameInterpolation)
                        + cent->currentState.u.turret.gunAngles[0]);
                    volumeScale = (float)((float)((float)(p_nextState->lerp.u.turret.gunAngles[1]
                        - cent->currentState.u.turret.gunAngles[1])
                        * LocalClientGlobals->frameInterpolation)
                        + cent->currentState.u.turret.gunAngles[1]);
                    iassert((lerp >= 0.0f) && (lerp <= 1.0f));
                    SND_PlayBlendedSoundAliases(
                        alias0,
                        alias1,
                        lerp,
                        volumeScale,
                        p_nextState->number,
                        cent->pose.origin,
                        0,
                        SASYS_CGAME);
                }
            }
        }
    }
}

FxEffect *__cdecl CG_StartFx(int localClientNum, centity_s *cent, int startAtTime)
{
    int scale; // r31
    const FxEffectDef *v7; // r31
    float v9[8][3]; // [sp+50h] [-60h] BYREF

    AnglesToAxis(cent->nextState.lerp.apos.trBase, v9);
    scale = cent->nextState.un1.scale;
    if (!cent->nextState.un1.scale || cent->nextState.un1.scale > 0x63u)
        MyAssertHandler(
            "c:\\trees\\cod3\\cod3src\\src\\cgame\\cg_ents.cpp",
            943,
            0,
            "fxId not in [1, MAX_EFFECT_NAMES - 1]\n\t%i not in [%i, %i]",
            cent->nextState.un1.scale,
            1,
            99);
    vassert((localClientNum == 0), "(localClientNum) = %i", localClientNum);
    v7 = cgsArray[0].fxs[scale];
    if (!v7)
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\cgame\\cg_ents.cpp", 946, 0, "%s", "fxDef");
    return FX_SpawnOrientedEffect(localClientNum, v7, startAtTime, cent->pose.origin, v9, 0x87Fu);
}

void __cdecl CG_Fx(int localClientNum, centity_s *cent)
{
    FxEffect *effect; // r4
    FxEffect *started; // r3

    if (cent->pose.actor.proneType != cent->nextState.time2)
    {
        effect = cent->pose.fx.effect;
        if (effect)
            FX_ThroughWithEffect(localClientNum, effect);
        cent->pose.actor.proneType = 0;
        cent->pose.fx.effect = 0;
        started = CG_StartFx(localClientNum, cent, cent->nextState.time2);
        cent->pose.fx.effect = started;
        if (started)
            cent->pose.actor.proneType = cent->nextState.time2;
    }
}

void __cdecl CG_LoopFx(int localClientNum, centity_s *cent)
{
    int period; // r28
    FxEffect *started; // r3
    int v6; // r10

    vassert((localClientNum == 0), "(localClientNum) = %i", localClientNum);
    if (cent->nextState.lerp.u.turret.gunAngles[0] == 0.0
        || (float)((float)((float)(cent->pose.origin[1] - cgArray[0].predictedPlayerState.origin[1])
            * (float)(cent->pose.origin[1] - cgArray[0].predictedPlayerState.origin[1]))
            + (float)((float)((float)(cent->pose.origin[0] - cgArray[0].predictedPlayerState.origin[0])
                * (float)(cent->pose.origin[0] - cgArray[0].predictedPlayerState.origin[0]))
                + (float)((float)(cent->pose.origin[2] - cgArray[0].predictedPlayerState.origin[2])
                    * (float)(cent->pose.origin[2] - cgArray[0].predictedPlayerState.origin[2])))) < (double)(float)(cent->nextState.lerp.u.turret.gunAngles[0] * cent->nextState.lerp.u.turret.gunAngles[0]))
    {
        period = cent->nextState.lerp.u.loopFx.period;
        iassert(period > 0);
        if (!cent->pose.fx.effect)
        {
            started = CG_StartFx(localClientNum, cent, cgArray[0].time);
            cent->pose.fx.effect = started;
            if (!started)
                return;
            cent->pose.actor.proneType = cgArray[0].time + period;
        }
        if (cgArray[0].time >= cent->pose.actor.proneType)
        {
            do
            {
                FX_RetriggerEffect(localClientNum, cent->pose.fx.effect, cent->pose.actor.proneType);
                v6 = cent->pose.actor.proneType + period;
                cent->pose.actor.proneType = v6;
            } while (cgArray[0].time >= v6);
        }
    }
}

void __cdecl CG_ClampPrimaryLightOrigin(GfxLight *light, const ComPrimaryLight *refLight)
{
    double v2; // fp13
    double v3; // fp12
    double v4; // fp11
    double v5; // fp0
    double v6; // fp0

    v2 = (float)(light->origin[1] - refLight->origin[1]);
    v3 = (float)(light->origin[2] - refLight->origin[2]);
    v4 = (float)(light->origin[0] - refLight->origin[0]);
    v5 = (float)((float)((float)v4 * (float)v4) + (float)((float)((float)v3 * (float)v3) + (float)((float)v2 * (float)v2)));
    if (v5 >= (float)(refLight->translationLimit * refLight->translationLimit))
    {
        v6 = (float)(refLight->translationLimit / (float)sqrtf(v5));
        light->origin[0] = (float)((float)v6 * (float)(light->origin[0] - refLight->origin[0])) + refLight->origin[0];
        light->origin[1] = (float)((float)v6 * (float)v2) + refLight->origin[1];
        light->origin[2] = (float)((float)v3 * (float)v6) + refLight->origin[2];
    }
}

void __cdecl CG_ClampPrimaryLightDir(GfxLight *light, const ComPrimaryLight *refLight)
{
    double rotationLimit; // fp12
    double v5; // fp11
    double v6; // fp0
    double v7; // fp10
    double v8; // fp9
    const char *v9; // r3
    double v10; // [sp+30h] [-40h]

    rotationLimit = refLight->rotationLimit;
    v5 = (float)((float)(light->dir[0] * refLight->dir[0])
        + (float)((float)(light->dir[2] * refLight->dir[2]) + (float)(light->dir[1] * refLight->dir[1])));
    if (v5 < rotationLimit)
    {
        v6 = sqrtf((float)((float)-(float)((float)(refLight->rotationLimit * refLight->rotationLimit) - (float)1.0)
            / (float)-(float)((float)((float)v5 * (float)v5) - (float)1.0)));
        v7 = (float)((float)((float)(refLight->dir[1] * (float)-v5) + light->dir[1]) * (float)v6);
        v8 = (float)((float)((float)(refLight->dir[2] * (float)-v5) + light->dir[2]) * (float)v6);
        light->dir[0] = (float)((float)((float)(refLight->dir[0] * (float)-v5) + light->dir[0]) * (float)v6)
            + (float)(refLight->rotationLimit * refLight->dir[0]);
        light->dir[1] = (float)(refLight->dir[1] * (float)rotationLimit) + (float)v7;
        light->dir[2] = (float)(refLight->dir[2] * (float)rotationLimit) + (float)v8;

        iassert(Vec3IsNormalized(light->dir));
        iassert(I_fabs(Vec3Dot(light->dir, refLight->dir) - refLight->rotationLimit) <= 0.001f);
    }
}

void CG_PrimaryLight(int localClientNum, centity_s *cent) {
    iassert(cent->nextState.eType == ET_PRIMARY_LIGHT);
    iassert(cent->nextState.index.item != PRIMARY_LIGHT_NONE);
    iassert(comWorld.isInUse);
    bcassert(cent->nextState.index.primaryLight, Com_GetPrimaryLightCount());

    vassert((localClientNum == 0), "(localClientNum) = %i", localClientNum);

    unsigned int lightIndex = cent->nextState.index.item;
    LerpEntityState *p_currentState = &cent->currentState;
    LerpEntityState *p_lerp = &cent->nextState.lerp;

    GfxLight *light = (GfxLight *)((char *)cgArray[0].refdef.primaryLights + __ROL4__(lightIndex, 6));
    const ComPrimaryLight *primaryLight = Com_GetPrimaryLight(lightIndex);
    float frameInterp = cgArray[0].frameInterpolation;

    // Decode color from current and lerp states (colorAndExp is [R, G, B, Exp])
    float cR_current = p_currentState->u.primaryLight.colorAndExp[0] * 0.0039215689f;
    float cG_current = p_currentState->u.primaryLight.colorAndExp[1] * 0.0039215689f;
    float cB_current = p_currentState->u.primaryLight.colorAndExp[2] * 0.0039215689f;

    float cR_lerp = p_lerp->u.primaryLight.colorAndExp[0] * 0.0039215689f;
    float cG_lerp = p_lerp->u.primaryLight.colorAndExp[1] * 0.0039215689f;
    float cB_lerp = p_lerp->u.primaryLight.colorAndExp[2] * 0.0039215689f;

    // GunAngles[1] used to scale color
    float scale_current = p_currentState->u.turret.gunAngles[1];
    float scale_lerp = p_lerp->u.turret.gunAngles[1];

    // Interpolate color
    light->color[0] = ((cR_lerp * scale_lerp - cR_current * scale_current) * frameInterp) + (cR_current * scale_current);
    light->color[1] = ((cG_lerp * scale_lerp - cG_current * scale_current) * frameInterp) + (cG_current * scale_current);
    light->color[2] = ((cB_lerp * scale_lerp - cB_current * scale_current) * frameInterp) + (cB_current * scale_current);

    // Handle light direction
    if (primaryLight->rotationLimit < 1.0f) {
        float angles[4];
        BG_EvaluateTrajectory(&p_lerp->apos, cgArray[0].time, angles);
        AngleVectors(angles, light->dir, NULL, NULL);

        // Flip direction
        light->dir[0] = -light->dir[0];
        light->dir[1] = -light->dir[1];
        light->dir[2] = -light->dir[2];

        if (primaryLight->rotationLimit > -1.0f) {
            CG_ClampPrimaryLightDir(light, primaryLight);
        }
    }

    // Handle light position
    if (primaryLight->translationLimit > 0.0f) {
        BG_EvaluateTrajectory(&p_lerp->pos, cgArray[0].time, light->origin);
        CG_ClampPrimaryLightOrigin(light, primaryLight);
    }

    // Interpolate radius (stored in gunAngles[2])
    float radius_current = p_currentState->u.turret.gunAngles[2];
    float radius_lerp = p_lerp->u.turret.gunAngles[2];
    light->radius = (radius_lerp - radius_current) * frameInterp + radius_current;

    // Interpolate FOV
    float fovOuter_current = p_currentState->u.primaryLight.cosHalfFovOuter;
    float fovOuter_lerp = p_lerp->u.primaryLight.cosHalfFovOuter;
    light->cosHalfFovOuter = (fovOuter_lerp - fovOuter_current) * frameInterp + fovOuter_current;

    float fovInner_current = p_currentState->u.primaryLight.cosHalfFovInner;
    float fovInner_lerp = p_lerp->u.primaryLight.cosHalfFovInner;
    light->cosHalfFovInner = (fovInner_lerp - fovInner_current) * frameInterp + fovInner_current;

    // Interpolate exponent (stored in colorAndExp[3])
    int exp_current = p_currentState->u.primaryLight.colorAndExp[3];
    int exp_lerp = p_lerp->u.primaryLight.colorAndExp[3];
    light->exponent = (int)((exp_lerp - exp_current) * frameInterp + exp_current);

    // Final assertion
    iassert(light->cosHalfFovOuter > 0.0f &&
        light->cosHalfFovOuter < light->cosHalfFovInner &&
        light->cosHalfFovInner <= 1.0f);
}


void __cdecl CG_InterpolateEntityOrigin(const cg_s *cgameGlob, centity_s *cent)
{
    float frameInterpolation; // fp31
    float current[3]; // [sp+50h] [-40h] BYREF v9
    float next[3]; // [sp+60h] [-30h] BYREF v12

    iassert(cgameGlob->nextSnap);

    frameInterpolation = cgameGlob->frameInterpolation;

    BG_EvaluateTrajectory(&cent->currentState.pos, cgameGlob->snap->serverTime, current);
    BG_EvaluateTrajectory(&cent->nextState.lerp.pos, cgameGlob->nextSnap->serverTime, next);

    cent->pose.origin[0] = ((next[0] - current[0]) * frameInterpolation) + current[0];
    cent->pose.origin[1] = ((next[1] - current[1]) * frameInterpolation) + current[1];
    cent->pose.origin[2] = ((next[2] - current[2]) * frameInterpolation) + current[2];
}

void __cdecl CG_InterpolateEntityAngles(const cg_s *cgameGlob, centity_s *cent)
{
    double frameInterpolation; // fp31
    float v5[4]; // [sp+50h] [-40h] BYREF
    float v6[4]; // [sp+60h] [-30h] BYREF

    iassert(cgameGlob->nextSnap);
    frameInterpolation = cgameGlob->frameInterpolation;
    BG_EvaluateTrajectory(&cent->currentState.apos, cgameGlob->snap->serverTime, v6);
    BG_EvaluateTrajectory(&cent->nextState.lerp.apos, cgameGlob->nextSnap->serverTime, v5);
    cent->pose.angles[0] = LerpAngle(v6[0], v5[0], frameInterpolation);
    cent->pose.angles[1] = LerpAngle(v6[1], v5[1], frameInterpolation);
    cent->pose.angles[2] = LerpAngle(v6[2], v5[2], frameInterpolation);
}

void __cdecl CG_CreatePhysicsObject(int localClientNum, centity_s *cent)
{
    const DObj_s *ClientDObj; // r29
    PhysPreset *PhysPreset; // r28
    const char *v6; // r3
    dxBody *v7; // r30
    const char *Name; // r3
    double v9; // fp0
    double v10; // fp1
    double v13; // fp13
    float v14[4]; // [sp+50h] [-70h] BYREF
    float v15[4]; // [sp+60h] [-60h] BYREF
    float v16[4]; // [sp+70h] [-50h] BYREF
    float v17[16]; // [sp+80h] [-40h] BYREF

    v14[0] = 0.0;
    v14[1] = 0.0;
    v14[2] = 0.0;
    v15[0] = cent->currentState.pos.trBase[0];
    v15[1] = cent->currentState.pos.trBase[1];
    v15[2] = cent->currentState.pos.trBase[2];
    AnglesToQuat(cent->currentState.apos.trBase, v17);
    ClientDObj = Com_GetClientDObj(cent->nextState.number, localClientNum);
    if (!ClientDObj)
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\cgame\\cg_ents.cpp", 1151, 0, "%s", "obj");
    PhysPreset = DObjGetPhysPreset(ClientDObj);
    if (PhysPreset)
    {
        Sys_EnterCriticalSection(CRITSECT_PHYSICS);
        v7 = Phys_ObjCreate(PHYS_WORLD_FX, v15, v17, v14, PhysPreset);
        if (v7)
        {
            DObjPhysicsSetCollisionFromXModel(ClientDObj, PHYS_WORLD_FX, v7);
            v9 = cent->currentState.apos.trDelta[2];

            v10 = sqrtf((float)((float)(cent->currentState.apos.trDelta[0] * cent->currentState.apos.trDelta[0])
                + (float)((float)(cent->currentState.apos.trDelta[2] * cent->currentState.apos.trDelta[2])
                    + (float)(cent->currentState.apos.trDelta[1] * cent->currentState.apos.trDelta[1]))));

            // aislop
            //float _FP10 = -v10;
            //__asm { fsel      f10, f10, f11, f1 }
            //v13 = (float)(cent->currentState.apos.trDelta[1] * (float)((float)1.0 / (float)_FP10));
            //v16[0] = (float)((float)1.0 / (float)_FP10) * cent->currentState.apos.trDelta[0];
            //v16[1] = v13;
            //v16[2] = (float)v9 * (float)((float)1.0 / (float)_FP10);

            {
                // PPC: _FP10 = -v10; fsel f10, f10, f11, f1
                // v10 = sqrtf(...) >= 0, so -v10 <= 0; only >= 0 when v10 == 0.
                // fsel result picks safe divisor when angular speed is zero (avoids 1/0).
                // When v10 == 0 trDelta is (0,0,0), so the normalized axis stays (0,0,0).
                float reciprocal = (v10 > 0.0f) ? (1.0f / v10) : 0.0f;
                v13 = cent->currentState.apos.trDelta[1] * reciprocal;
                v16[0] = reciprocal * cent->currentState.apos.trDelta[0];
                v16[1] = v13;
                v16[2] = v9 * reciprocal;
            }
            

            Phys_ObjBulletImpact(PHYS_WORLD_FX, v7, cent->currentState.pos.trDelta, v16, v10, PhysPreset->bulletForceScale);
            Sys_LeaveCriticalSection(CRITSECT_PHYSICS);
            cent->pose.physObjId = (uintptr_t)v7;
        }
        else
        {
            Name = DObjGetName(ClientDObj);
            Com_PrintWarning(CON_CHANNEL_ERROR, "Failed to create physics object for '%s'.\n", Name);
            cent->pose.physObjId = -1;
            Sys_LeaveCriticalSection(CRITSECT_PHYSICS);
        }
    }
    else
    {
        cent->pose.physObjId = -1;
        v6 = DObjGetName(ClientDObj);
        Com_PrintWarning(CON_CHANNEL_ERROR, "Failed to create physics object for '%s'.  No physics preset.\n", v6);
    }
}

void __cdecl CG_UpdatePhysicsPose(centity_s *cent)
{
    uintptr_t physObjId; // r11
    float v3[4]; // [sp+50h] [-20h] BYREF

    physObjId = cent->pose.physObjId;
    if (!physObjId || physObjId == -1)
        MyAssertHandler(
            "c:\\trees\\cod3\\cod3src\\src\\cgame\\cg_ents.cpp",
            1195,
            0,
            "%s",
            "cent->pose.physObjId != PHYS_OBJ_ID_NULL && cent->pose.physObjId != PHYS_OBJ_ID_DEAD");
    Sys_EnterCriticalSection(CRITSECT_PHYSICS);
    Phys_ObjGetInterpolatedState(PHYS_WORLD_FX, (dxBody*)cent->pose.physObjId, cent->pose.origin, v3);
    Sys_LeaveCriticalSection(CRITSECT_PHYSICS);
    UnitQuatToAngles(v3, cent->pose.angles);
}

int __cdecl CG_ExpiredLaunch(int localClientNum, centity_s *cent)
{
    int result; // r3

    vassert((localClientNum == 0), "(localClientNum) = %i", localClientNum);
    if (cent->pose.physObjId || cgArray[0].time <= cent->currentState.pos.trTime + 1000)
        return 0;
    result = 1;
    cent->pose.physObjId = -1;
    return result;
}

void __cdecl CG_CalcEntityPhysicsPositions(int localClientNum, centity_s *cent)
{
    iassert(cent);
    iassert(cent->currentState.pos.trType == TR_PHYSICS && cent->currentState.apos.trType == TR_PHYSICS);
    if (Com_GetClientDObj(cent->nextState.number, localClientNum)
        && !(unsigned __int8)CG_ExpiredLaunch(localClientNum, cent))
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

void __cdecl CG_SaveEntityPhysics(centity_s *cent, SaveGame *save)
{
    const DObj_s *obj; // r28
    uintptr_t physObjId; // r11
    char v6; // r11
    bool v7; // zf
    const char *modelName; // r29
    char v11; // [sp+50h] [-40h] BYREF

    iassert(cent);
    iassert(save);

    obj = Com_GetClientDObj(cent->nextState.number, 0);
    if (!obj || (physObjId = cent->pose.physObjId) == 0 || (v7 = physObjId != -1, v6 = 1, !v7))
        v6 = 0;
    v11 = v6;
    SaveMemory_SaveWrite(&v11, 1, save);
    if (v11)
    {
        Phys_ObjSave((dxBody*)cent->pose.physObjId, SaveMemory_GetMemoryFile(save));
        iassert(obj);
        modelName = DObjGetName(obj);
        iassert(modelName);
        MemFile_WriteCString(SaveMemory_GetMemoryFile(save), modelName);
    }
}

void __cdecl CG_LoadEntityPhysics(centity_s *cent, SaveGame *save)
{
    const char *CString; // r3
    const XModel *xmodel; // r29
    char v8; // [sp+50h] [-30h] BYREF

    iassert(cent);
    iassert(save);

    v8 = 0;
    SaveMemory_LoadRead(&v8, 1, save);

    if (v8)
    {
        cent->pose.physObjId = (uintptr_t)Phys_ObjLoad(PHYS_WORLD_FX, SaveMemory_GetMemoryFile(save));
        CString = MemFile_ReadCString(SaveMemory_GetMemoryFile(save));
        xmodel = R_RegisterModel(CString);
        iassert(xmodel);
        Phys_ObjSetCollisionFromXModel(xmodel, PHYS_WORLD_FX, (dxBody*)cent->pose.physObjId);
    }
}

void __cdecl CG_CreateRagdollObject(int localClientNum, centity_s *cent)
{
    cent->pose.ragdollHandle = Ragdoll_CreateRagdollForDObj(localClientNum, 0, cent->nextState.number, 0, 1);
    cent->pose.isRagdoll = 1;
}

void __cdecl CG_UpdateRagdollPose(centity_s *cent)
{
    if (!cent->pose.ragdollHandle)
        MyAssertHandler(
            "c:\\trees\\cod3\\cod3src\\src\\cgame\\cg_ents.cpp",
            1314,
            0,
            "%s",
            "cent->pose.ragdollHandle != RAGDOLL_INVALID");
    Ragdoll_GetRootOrigin(cent->pose.ragdollHandle, cent->pose.origin);
}

void __cdecl CG_CalcEntityRagdollPositions(int localClientNum, centity_s *cent)
{
    iassert(cent);
    if (!(unsigned __int8)Com_IsRagdollTrajectory(&cent->currentState.pos)
        && !(unsigned __int8)Com_IsRagdollTrajectory(&cent->currentState.apos))
    {
        MyAssertHandler(
            "c:\\trees\\cod3\\cod3src\\src\\cgame\\cg_ents.cpp",
            1322,
            0,
            "%s",
            "Com_IsRagdollTrajectory( &cent->currentState.pos ) || Com_IsRagdollTrajectory( &cent->currentState.apos )");
    }
    if (!cent->pose.ragdollHandle)
    {
        cent->pose.ragdollHandle = Ragdoll_CreateRagdollForDObj(localClientNum, 0, cent->nextState.number, 0, 1);
        cent->pose.isRagdoll = 1;
    }
    if (cent->pose.ragdollHandle)
        CG_UpdateRagdollPose(cent);
}

/*
===============
CG_CalcEntityLerpPositions

===============
*/
void __cdecl CG_CalcEntityLerpPositions(int localClientNum, centity_s *cent)
{
    vassert((localClientNum == 0), "(localClientNum) = %i", localClientNum);
    if (cent->currentState.pos.trType == TR_PHYSICS)
    {
        CG_CalcEntityPhysicsPositions(localClientNum, cent);
    }
    else if ((unsigned __int8)Com_IsRagdollTrajectory(&cent->currentState.pos))
    {
        CG_InterpolateEntityOrigin(cgArray, cent);
        CG_CalcEntityRagdollPositions(localClientNum, cent);
    }
    else
    {
        CG_InterpolateEntityOrigin(cgArray, cent);
        if (cent->currentState.pos.trType != TR_INTERPOLATE && cent != &cgArray[0].predictedPlayerEntity)
            CG_AdjustPositionForMover(
                localClientNum,
                cent->pose.origin,
                cent->nextState.groundEntityNum,
                cgArray[0].snap->serverTime,
                cgArray[0].time,
                cent->pose.origin,
                0);
        if (cent->currentState.apos.trType == TR_INTERPOLATE)
            CG_InterpolateEntityAngles(cgArray, cent);
        else
            BG_EvaluateTrajectory(&cent->currentState.apos, cgArray[0].time, cent->pose.angles);
    }
}

void __cdecl CG_DObjCalcBone(const cpose_t *pose, DObj_s *obj, int boneIndex)
{
    const DObj_s *v6; // r3
    bool v7; // zf
    int v8[16]; // [sp+50h] [-40h] BYREF

    iassert(obj);
    iassert(pose);
    DObjLock(obj);
    v7 = CL_DObjCreateSkelForBone(obj, boneIndex) != 0;
    v6 = obj;
    if (!v7)
    {
        DObjGetHierarchyBits(obj, boneIndex, v8);
        CG_DoControllers(pose, obj, v8);
        DObjCalcSkel(obj, v8);
        v6 = obj;
    }
    DObjUnlock((DObj_s *)v6);
}

void __cdecl CG_DrawEntEqDebug(const centity_s *cent)
{
    int eType; // r3
    int number; // r30
    const char *EntityTypeName; // r3
    char *txt; // r6
    const float *color; // r5

    if (snd_drawEqEnts->current.enabled)
    {
        eType = cent->nextState.eType;
        switch (cent->nextState.eType)
        {
        case 3u:
        case 6u:
        case 0xAu:
        case 0xEu:
            number = cent->nextState.number;
            EntityTypeName = BG_GetEntityTypeName(eType);
            txt = va("%s: #%i", EntityTypeName, number);
            if ((cent->currentState.eFlags & 0x200000) != 0)
                color = colorRed;
            else
                color = colorLtGreen;
            CG_DebugStarWithText(cent->pose.origin, color, color, txt, 1.0, 1);
            break;
        default:
            return;
        }
    }
}

void __cdecl CG_ClearUnion(int localClientNum, centity_s *cent)
{
    FxEffect *effect; // r4
    CEntActorInfo *v4; // r11
    int v5; // ctr

    switch (cent->pose.eTypeUnion)
    {
    case 7u:
    case 8u:
        effect = cent->pose.fx.effect;
        if (effect)
            FX_ThroughWithEffect(localClientNum, effect);
        cent->pose.actor.proneType = 0;
        cent->pose.fx.effect = 0;
        break;
    case 0xAu:
        cent->pose.actor.proneType = 0;
        *(_QWORD *)&cent->pose.fx.effect = 0;
        cent->pose.actor.height = 0.0;
        break;
    case 0xBu:
    case 0xDu:
        v4 = &cent->pose.actor;
        v5 = 10;
        do
        {
            v4->proneType = 0;
            v4 = (CEntActorInfo *)((char *)v4 + 4);
            --v5;
        } while (v5);
        break;
    case 0xEu:
    case 0x10u:
        cent->pose.actor.proneType = 0;
        *(_QWORD *)&cent->pose.fx.effect = 0;
        cent->pose.actor.height = 0.0;
        break;
    default:
        break;
    }
    cent->pose.eTypeUnion = 0;
}

void __cdecl CG_SetUnionType(int localClientNum, centity_s *cent)
{
    unsigned __int8 eType; // r11

    eType = cent->nextState.eType;
    switch (eType)
    {
    case 7u:
    case 8u:
    case 0xAu:
    case 0xBu:
    case 0xDu:
    case 0xEu:
    case 0x10u:
        break;
    default:
        eType = 0;
        break;
    }
    cent->pose.eTypeUnion = eType;
}

void __cdecl CG_UpdatePoseUnion(int localClientNum, centity_s *cent)
{
    unsigned __int8 eType; // r11

    CG_ClearUnion(localClientNum, cent);
    eType = cent->nextState.eType;
    switch (eType)
    {
    case 7u:
    case 8u:
    case 0xAu:
    case 0xBu:
    case 0xDu:
    case 0xEu:
    case 0x10u:
        break;
    default:
        eType = 0;
        break;
    }
    cent->pose.eTypeUnion = eType;
}

void __cdecl CG_ProcessEntity(int localClientNum, centity_s *cent)
{
    if (cent->nextState.loopSound)
        CG_AddEntityLoopSound(localClientNum, cent);
    if (cent->nextState.eType != cent->pose.eTypeUnion)
        CG_UpdatePoseUnion(localClientNum, cent);

    CG_DrawEntEqDebug(cent);

    switch (cent->nextState.eType)
    {
    case ET_GENERAL:
        CG_General(localClientNum, cent);
        break;
    case ET_PLAYER:
    case ET_INVISIBLE:
        return;
    case ET_ITEM:
        CG_Item(cent);
        break;
    case ET_MISSILE:
        CG_Missile(localClientNum, cent);
        break;
    case ET_SCRIPTMOVER:
        CG_ScriptMover(localClientNum, cent);
        break;
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
    case ET_VEHICLE:
    case ET_VEHICLE_CORPSE:
        CG_Vehicle(localClientNum, cent);
        break;
    case ET_ACTOR:
    case ET_ACTOR_CORPSE:
        CG_Actor(localClientNum, cent);
        break;
    case ET_ACTOR_SPAWNER:
        CG_ActorSpawner(cent);
        break;
    default:
        Com_Error(ERR_DROP, "Bad entity type %i", cent->nextState.eType);
        break;
    }
}

void __cdecl CG_SaveEntityFX(centity_s *cent, SaveGame *save)
{
    int eType; // r11
    int v5; // r11
    bool v6; // zf
    FxEffect *effect; // r4
    int v8; // [sp+50h] [-20h] BYREF
    int ClientEffectIndex; // [sp+54h] [-1Ch] BYREF

    eType = cent->nextState.eType;
    if (eType == ET_FX || (v6 = eType != ET_LOOP_FX, v5 = 0, !v6))
        v5 = 1;
    v8 = v5;
    SaveMemory_SaveWrite(&v8, 4, save);
    if (v8)
    {
        SaveMemory_SaveWrite(&cent->pose.fx.triggerTime, 4, save);
        effect = cent->pose.fx.effect;
        if (effect)
            ClientEffectIndex = FX_GetClientEffectIndex(0, effect);
        else
            ClientEffectIndex = -1;
        SaveMemory_SaveWrite(&ClientEffectIndex, 4, save);
    }
}

void __cdecl CG_LoadEntityFX(centity_s *cent, SaveGame *save)
{
    int v4; // [sp+50h] [-20h] BYREF
    int v5; // [sp+54h] [-1Ch] BYREF

    SaveMemory_LoadRead(&v4, 4, save);

    if (v4)
    {
        SaveMemory_LoadRead(&cent->pose.fx.triggerTime, 4, save);
        SaveMemory_LoadRead(&v5, 4, save);
        if (v5 == -1)
            cent->pose.fx.effect = 0;
        else
            cent->pose.fx.effect = FX_GetClientEffectByIndex(0, v5);
    }
}

void __cdecl CG_SaveEntity(unsigned int entnum, SaveGame *save)
{
    centity_s *v4; // r31

    iassert(save);
    if (entnum >= 0x880)
    {
        MyAssertHandler(
            "c:\\trees\\cod3\\cod3src\\src\\cgame\\cg_ents.cpp",
            1629,
            0,
            "entnum doesn't index MAX_GENTITIES\n\t%i not in [0, %i)",
            entnum,
            2176);
        MyAssertHandler(
            "c:\\trees\\cod3\\cod3src\\src\\cgame\\cg_local.h",
            932,
            0,
            "entityIndex doesn't index MAX_GENTITIES\n\t%i not in [0, %i)",
            entnum,
            2176);
    }
    v4 = &cg_entitiesArray[0][entnum];
    if (!v4->nextValid)
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\cgame\\cg_ents.cpp", 1632, 0, "%s", "cent->nextValid");
    SaveMemory_SaveWrite(&v4->previousEventSequence, 4, save);
    CG_SaveEntityFX(v4, save);
    CG_SaveEntityPhysics(v4, save);
}

void __cdecl CG_LoadEntity(unsigned int entnum, SaveGame *save)
{
    centity_s *v4; // r31

    iassert(save);
    if (entnum >= 0x880)
    {
        MyAssertHandler(
            "c:\\trees\\cod3\\cod3src\\src\\cgame\\cg_ents.cpp",
            1649,
            0,
            "entnum doesn't index MAX_GENTITIES\n\t%i not in [0, %i)",
            entnum,
            2176);
        MyAssertHandler(
            "c:\\trees\\cod3\\cod3src\\src\\cgame\\cg_local.h",
            932,
            0,
            "entityIndex doesn't index MAX_GENTITIES\n\t%i not in [0, %i)",
            entnum,
            2176);
    }
    v4 = &cg_entitiesArray[0][entnum];
    SaveMemory_LoadRead(&v4->previousEventSequence, 4, save);
    CG_LoadEntityFX(v4, save);
    CG_LoadEntityPhysics(v4, save);
}

void __cdecl CG_SaveEntities(SaveGame *save)
{
    int entityIndex; // r7
    int v3; // r31
    unsigned int v4[16]; // [sp+50h] [-40h] BYREF

    entityIndex = 0;
    v4[0] = 0;
    do
    {
        v3 = entityIndex;
        if ((unsigned int)entityIndex >= 0x880)
        {
            MyAssertHandler(
                "c:\\trees\\cod3\\cod3src\\src\\cgame\\cg_local.h",
                932,
                0,
                "entityIndex doesn't index MAX_GENTITIES\n\t%i not in [0, %i)",
                entityIndex,
                2176);
            entityIndex = v4[0];
        }
        if (cg_entitiesArray[0][v3].nextValid)
        {
            SaveMemory_SaveWrite(v4, 4, save);
            CG_SaveEntity(v4[0], save);
            entityIndex = v4[0];
        }
        v4[0] = ++entityIndex;
    } while (entityIndex < 2176);
    v4[0] = -1;
    SaveMemory_SaveWrite(v4, 4, save);
}

void __cdecl CG_LoadEntities(SaveGame *save)
{
    signed int i; // r5
    int entityIndex; // [sp+50h] [-20h] BYREF

    SaveMemory_LoadRead(&entityIndex, 4, save);
    for (i = entityIndex; entityIndex >= 0; i = entityIndex)
    {
        if (i >= MAX_GENTITIES)
        {
            Com_Error(ERR_DROP, "G_LoadGame: entitynum out of range (%i, MAX = %i)", i, MAX_GENTITIES);
            i = entityIndex;
        }
        CG_LoadEntity(i, save);
        SaveMemory_LoadRead(&entityIndex, 4, save);
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

float *__cdecl CG_GetEntityOrigin(int localClientNum, int entnum)
{
    return CG_GetEntity(localClientNum, entnum)->pose.origin;
}

void __cdecl CG_GetPoseLightingHandle(const cpose_t *pose)
{
    ;
}

void __cdecl CG_PredictiveSkinCEntity(GfxSceneEntity *sceneEnt)
{
    GfxSceneEntityInfo v2; // r11
    int cullIn; // r10

    iassert(sceneEnt);

    if (!cl_freemove->current.integer)
    {
        v2.pose = sceneEnt->info.pose;
        cullIn = v2.pose->cullIn;
        if (cullIn == 1)
        {
            v2.pose->cullIn = 0;
            R_UpdateXModelBoundsDelayed(sceneEnt);
        }
        else if (cullIn == 2)
        {
            v2.pose->cullIn = 0;
            R_SkinGfxEntityDelayed(sceneEnt);
        }
    }
}

void __cdecl CG_AddPacketEntity(unsigned int localClientNum, unsigned int entnum)
{
    centity_s *Entity; // r3
    centity_s *v5; // r27
    float *origin; // r31
    double v7; // fp28
    double v8; // fp27
    double v9; // fp31
    double v10; // fp30
    double v11; // fp29
    double v12; // fp26
    char v13; // r11
    unsigned __int8 v14; // r28
    char v15; // r9
    float *v16; // r11
    double v17; // fp0
    double v18; // fp13
    const DObj_s *ClientDObj; // r3
    double Radius; // fp1
    int v21; // r11

    Entity = CG_GetEntity(localClientNum, entnum);
    v5 = Entity;
    origin = Entity->pose.origin;
    v7 = Entity->pose.angles[0];
    v8 = Entity->pose.angles[1];
    v9 = Entity->pose.origin[0];
    v10 = Entity->pose.origin[1];
    v11 = Entity->pose.origin[2];
    v12 = Entity->pose.angles[2];
    if (Entity->nextState.eType == ET_SCRIPTMOVER && Entity->nextState.solid == 0xFFFFFF)
    {
        CG_CalcEntityLerpPositions(localClientNum, Entity);
        if (*origin != v9
            || origin[1] != v10
            || origin[2] != v11
            || v5->pose.angles[0] != v7
            || v5->pose.angles[1] != v8
            || (v13 = 0, v5->pose.angles[2] != v12))
        {
            v13 = 1;
        }
        v14 = v13;
        if (v13)
            CG_UpdateBModelWorldBounds(localClientNum, v5, 0);
    }
    else
    {
        CG_CalcEntityLerpPositions(localClientNum, Entity);
        if (*origin != v9
            || origin[1] != v10
            || origin[2] != v11
            || v5->pose.angles[0] != v7
            || v5->pose.angles[1] != v8
            || (v15 = 0, v5->pose.angles[2] != v12))
        {
            v15 = 1;
        }
        v14 = v15;
        v16 = cg_entityOriginArray[localClientNum][entnum];
        v17 = (float)(v16[1] - origin[1]);
        v18 = (float)(v16[2] - origin[2]);
        if ((float)((float)((float)(*v16 - *origin) * (float)(*v16 - *origin))
            + (float)((float)((float)v18 * (float)v18) + (float)((float)v17 * (float)v17))) > 256.0)
        {
            *v16 = *origin;
            v16[1] = origin[1];
            v16[2] = origin[2];
            ClientDObj = Com_GetClientDObj(entnum, localClientNum);
            if (ClientDObj)
            {
                Radius = DObjGetRadius(ClientDObj);
                R_LinkDObjEntity(localClientNum, entnum, origin, (float)((float)Radius + (float)16.0));
            }
        }
    }
    if (v5->pose.physObjId == -1)
    {
        if (CG_IsEntityLinked(localClientNum, entnum))
            CG_UnlinkEntity(localClientNum, entnum);
    }
    else
    {
        if (CG_IsEntityLinked(localClientNum, entnum))
            v21 = v14;
        else
            v21 = CG_EntityNeedsLinked(localClientNum, entnum);
        if (v21)
            CG_LinkEntity(localClientNum, entnum);
        CG_ProcessEntity(localClientNum, v5);
    }
}

/*
===============
CG_AddPacketEntities

===============
*/
int __cdecl CG_AddPacketEntities(int localClientNum)
{
    int viewlocked_entNum; // r24
    int v3; // r25
    snapshot_s *nextSnap; // r11
    int v5; // r29
    int v6; // r30
    unsigned int v7; // r31
    centity_s *Entity; // r4

    //PIXBeginNamedEvent_Copy_NoVarArgs(0xFFFFFFFF, "add packet ents");
    //Profile_Begin(325);
    vassert((localClientNum == 0), "(localClientNum) = %i", localClientNum);
    cgArray[0].rumbleScale = 0.0;
    if ((cgArray[0].predictedPlayerState.eFlags & 0x300) != 0)
        viewlocked_entNum = cgArray[0].predictedPlayerState.viewlocked_entNum;
    else
        viewlocked_entNum = ENTITYNUM_NONE;
    v3 = 0;
    nextSnap = cgArray[0].nextSnap;
    v5 = 0;
    // add each entity sent over by the server
    if (cgArray[0].nextSnap->numEntities > 0)
    {
        do
        {
            v7 = nextSnap->entityNums[v5];
            Entity = CG_GetEntity(localClientNum, v7);
            if (Entity->nextState.eType < ET_EVENTS)
            {
                if (v7 == viewlocked_entNum)
                {
                    v3 = 1;
                    CG_CalcEntityLerpPositions(localClientNum, Entity);
                }
                else
                {
                    CG_AddPacketEntity(localClientNum, v7);
                }
            }
            nextSnap = cgArray[0].nextSnap;
            ++v5;
        } while (v5 < cgArray[0].nextSnap->numEntities);
    }
    //Profile_EndInternal(0);
    //PIXEndNamedEvent();
    return v3;
}

DObjAnimMat *__cdecl CG_DObjGetLocalBoneMatrix(const cpose_t *pose, DObj_s *obj, int boneIndex)
{
    DObjAnimMat *RotTransArray; // r3

    iassert(obj);
    //Profile_Begin(319);
    CG_DObjCalcBone(pose, obj, boneIndex);
    //Profile_EndInternal(0);
    RotTransArray = DObjGetRotTransArray(obj);
    if (RotTransArray)
        return &RotTransArray[boneIndex];
    else
        return 0;
}

DObjAnimMat *__cdecl CG_DObjGetLocalTagMatrix(const cpose_t *pose, DObj_s *obj, unsigned int tagName)
{
    unsigned __int8 boneIndex;

    iassert(obj);
    boneIndex = -2;
    if (DObjGetBoneIndex(obj, tagName, &boneIndex))
        return CG_DObjGetLocalBoneMatrix(pose, obj, boneIndex);
    return NULL;
}

int __cdecl CG_DObjGetWorldBoneMatrix(
    const cpose_t *pose,
    DObj_s *obj,
    int boneIndex,
    float (*tagMat)[3],
    float *origin)
{
    DObjAnimMat *mat; // r3

    iassert(obj);
    iassert(pose);

    mat = CG_DObjGetLocalBoneMatrix(pose, obj, boneIndex);
    if (!mat)
        return 0;

    LocalConvertQuatToMat(mat, tagMat);
    Vec3Add(mat->trans, CG_GetLocalClientGlobals(0)->refdef.viewOffset, origin);

    return 1;
}

int __cdecl CG_DObjGetWorldTagMatrix(
    const cpose_t *pose,
    DObj_s *obj,
    unsigned int tagName,
    float (*tagMat)[3],
    float *origin)
{
    DObjAnimMat *mat; // r3

    iassert(obj);
    iassert(pose);

    mat = CG_DObjGetLocalTagMatrix(pose, obj, tagName);

    if (!mat)
    {
        return 0;
    }

    LocalConvertQuatToMat(mat, tagMat);
    Vec3Add(mat->trans, CG_GetLocalClientGlobals(0)->refdef.viewOffset, origin);
    return 1;
}

int __cdecl CG_DObjGetWorldTagPos(const cpose_t *pose, DObj_s *obj, unsigned int tagName, float *pos)
{
    DObjAnimMat *mat;

    iassert(obj);
    iassert(pose);

    mat = CG_DObjGetLocalTagMatrix(pose, obj, tagName);

    if (!mat)
    {
        return 0;
    }

    Vec3Add(mat->trans, CG_GetLocalClientGlobals(0)->refdef.viewOffset, pos);

    return 1;
}

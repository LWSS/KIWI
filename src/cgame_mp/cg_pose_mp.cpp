#ifndef KISAK_MP
#error This File is MultiPlayer Only
#endif

#include <universal/q_shared.h>
#include "cg_local_mp.h"
#include "cg_public_mp.h"
#include <xanim/dobj_utils.h>
#include <ragdoll/ragdoll.h>
#include <gfx_d3d/r_scene.h>
#include <universal/profile.h>


void CG_VehPoseControllers(const cpose_t *pose, const DObj_s *obj, int *partBits)
{
    float bodyAngles[3] = { 0.0f, 0.0f, 0.0f };
    float turretAngles[3] = { 0.0f, 0.0f, 0.0f };
    float barrelAngles[3] = { 0.0f, 0.0f, 0.0f };
    float steerAngles[3] = { 0.0f, 0.0f, 0.0f };
    float axis[3][3];
    float wheelPos[3];
    float relativePos[3];
    float offset[3];
    float fraction;
    float travel;
    float minTravel;
    const XModel *model;
    const DObjAnimMat *basePose;
    const float *basePos;
    uint boneIndex;
    int wheel;
    int component;

    iassert(obj);
    bodyAngles[0] = pose->vehicle.pitch * 0.0054931640625f;
    bodyAngles[2] = pose->vehicle.roll * 0.0054931640625f;
    turretAngles[1] = pose->vehicle.yaw * 0.0054931640625f;
    barrelAngles[0] = pose->vehicle.barrelPitch * 0.0054931640625f;
    if (pose->eType == ET_HELICOPTER)
        barrelAngles[2] = pose->vehicle.barrelRoll;
    steerAngles[1] = pose->vehicle.steerYaw * 0.0054931640625f;
    DObjSetLocalTag((DObj_s *)obj, partBits, pose->vehicle.tag_body, vec3_origin, bodyAngles);
    DObjSetLocalTag((DObj_s *)obj, partBits, pose->vehicle.tag_turret, vec3_origin, turretAngles);
    DObjSetLocalTag((DObj_s *)obj, partBits, pose->vehicle.tag_barrel, vec3_origin, barrelAngles);

    AnglesToAxis(pose->angles, axis);
    model = DObjGetModel(obj, 0);
    basePose = XModelGetBasePose(model);
    for (wheel = 0; wheel < ARRAY_COUNT(pose->vehicle.wheelBoneIndex); ++wheel)
    {
        boneIndex = pose->vehicle.wheelBoneIndex[wheel];
        if (boneIndex >= 0xFE || !DObjSetRotTransIndex((DObj_s *)obj, partBits, boneIndex))
            continue;
        iassert(boneIndex < XModelNumBones(model));
        basePos = basePose[boneIndex].trans;
        for (component = 0; component < 3; ++component)
        {
            wheelPos[component] = basePos[0] * axis[0][component]
                + basePos[1] * axis[1][component] + basePos[2] * axis[2][component]
                + pose->origin[component];
        }
        fraction = (float)(pose->vehicle.wheelFraction[wheel] * 0.00001525902189314365);
        travel = fraction * (pose->vehicle.time + 40.0f);
        minTravel = 40.0f - pose->vehicle.time;
        if (travel < minTravel)
            travel = minTravel;
        for (component = 0; component < 3; ++component)
        {
            wheelPos[component] += 40.0f * axis[2][component];
            wheelPos[component] -= travel * axis[2][component];
            relativePos[component] = wheelPos[component] - pose->origin[component];
        }
        for (component = 0; component < 3; ++component)
            offset[component] = Vec3Dot(relativePos, axis[component]) - basePos[component];
        DObjSetLocalTagInternal(obj, offset, steerAngles[1] != 0.0f && wheel < 2 ? steerAngles : NULL, boneIndex);
    }
}

void __cdecl CG_DoControllers(const cpose_t *pose, const DObj_s *obj, int *partBits)
{
    int setPartBits[4]; // [esp+34h] [ebp-10h] BYREF

    PROF_SCOPED("CG_DoControllers");

    DObjGetSetBones(obj, setPartBits);
    switch (pose->eType)
    {
    case ET_PLAYER:
        CG_Player_DoControllers(pose, obj, partBits);
        break;
    case ET_MG42:
        CG_mg42_DoControllers(pose, obj, partBits);
        break;
    case ET_HELICOPTER:
    case ET_VEHICLE:
        CG_VehPoseControllers(pose, obj, partBits);
        break;
    default:
        break;
    }
    CG_DoBaseOriginController(pose, obj, setPartBits);
    if (pose->isRagdoll && (pose->ragdollHandle || pose->killcamRagdollHandle))
        Ragdoll_DoControllers(pose, (DObj_s*)obj, partBits);
}

void __cdecl CG_Player_DoControllers(const cpose_t *pose, const DObj_s *obj, int *partBits)
{
    if (pose->fx.triggerTime)
        BG_Player_DoControllers(&pose->player, obj, partBits);
}

void __cdecl CG_mg42_DoControllers(const cpose_t *pose, const DObj_s *obj, int *partBits)
{
    float angles[3]; // [esp+10h] [ebp-10h] BYREF
    const float *viewAngles; // [esp+1Ch] [ebp-4h]

    if (pose->turret.playerUsing)
    {
        viewAngles = pose->turret.viewAngles;
        angles[0] = AngleDelta(viewAngles[0], pose->angles[0]);
        angles[1] = AngleDelta(viewAngles[1], pose->angles[1]);
    }
    else
    {
        //angles[0] = pose->turret.$9D88A49AD898204B3D6E378457DD8419::angles.pitch;
        angles[0] = pose->turret.angles.pitch;
        //angles[1] = pose->turret.$9D88A49AD898204B3D6E378457DD8419::angles.yaw;
        angles[1] = pose->turret.angles.yaw;
    }
    angles[2] = 0.0;
    DObjSetControlTagAngles((DObj_s*)obj, partBits, pose->turret.tag_aim, angles);
    DObjSetControlTagAngles((DObj_s *)obj, partBits, pose->turret.tag_aim_animated, angles);
    angles[0] = pose->turret.barrelPitch;
    angles[1] = 0.0;
    DObjSetControlTagAngles((DObj_s *)obj, partBits, pose->turret.tag_flash, angles);
}

void __cdecl CG_DoBaseOriginController(const cpose_t *pose, const DObj_s *obj, int *setPartBits)
{
    uint rootBoneMask; // [esp+90h] [ebp-7Ch]
    float baseQuat[4]; // [esp+94h] [ebp-78h] BYREF
    float viewOffset[3]; // [esp+A4h] [ebp-68h] BYREF
    float origin[3]; // [esp+B0h] [ebp-5Ch] BYREF
    int partIndex; // [esp+BCh] [ebp-50h]
    DObjAnimMat animMat; // [esp+C0h] [ebp-4Ch] BYREF
    int rootBoneCount; // [esp+E0h] [ebp-2Ch]
    uint maxHighIndex; // [esp+E4h] [ebp-28h]
    DObjAnimMat *mat; // [esp+E8h] [ebp-24h]
    uint highIndex; // [esp+ECh] [ebp-20h]
    int partBits[7];
    cg_s *cgameGlob;

    rootBoneCount = DObjGetRootBoneCount(obj);
    iassert(rootBoneCount);

    maxHighIndex = --rootBoneCount >> 5;
    for (highIndex = 0; highIndex < maxHighIndex; ++highIndex)
    {
        if (setPartBits[highIndex] != -1)
            goto notSet;
    }

    rootBoneMask = 0x7FFFFFFFu >> (rootBoneCount & 0x1F);
    if ((rootBoneMask | setPartBits[maxHighIndex]) == 0xFFFFFFFF)
        return;
notSet:
    mat = DObjGetRotTransArray(obj);
    if (mat)
    {
        AnglesToQuat(pose->angles, baseQuat);
        memset(partBits, 0, sizeof(partBits));
        partBits[3] = 0x80000000;
        cgameGlob = CG_GetLocalClientGlobals(R_GetLocalClientNum());
        viewOffset[0] = cgameGlob->refdef.viewOffset[0];
        viewOffset[1] = cgameGlob->refdef.viewOffset[1];
        viewOffset[2] = cgameGlob->refdef.viewOffset[2];
        partIndex = 0;
        while (partIndex <= rootBoneCount)
        {
            highIndex = partIndex >> 5;
            if ((setPartBits[partIndex >> 5] & partBits[3]) == 0)
            {
                if (DObjSetRotTransIndex((DObj_s*)obj, &partBits[3 - highIndex], partIndex))
                {
                    mat->quat[0] = baseQuat[0];
                    mat->quat[1] = baseQuat[1];
                    mat->quat[2] = baseQuat[2];
                    mat->quat[3] = baseQuat[3];

                    origin[0] = pose->origin[0];
                    origin[1] = pose->origin[1];
                    origin[2] = pose->origin[2];
                }
                else
                {
                    animMat.quat[0] = baseQuat[0];
                    animMat.quat[1] = baseQuat[1];
                    animMat.quat[2] = baseQuat[2];
                    animMat.quat[3] = baseQuat[3];
                    DObjSetTrans(&animMat, pose->origin);
                    float len = Vec4LengthSq(animMat.quat);
                    if (len == 0.0f)
                    {
                        animMat.quat[3] = 1.0f;
                        animMat.transWeight = 2.0f;
                    }
                    else
                    {
                        animMat.transWeight = 2.0f / len;
                    }
                    QuatMultiplyEquals(baseQuat, mat->quat);
                    MatrixTransformVectorQuatTrans(mat->trans, &animMat, origin);
                }
                Vec3Sub(origin, viewOffset, origin);
                DObjSetTrans(mat, origin);
            }
            ++partIndex;
            partBits[3] = (partBits[3] << 31) | ((uint)partBits[3] >> 1);
            ++mat;
        }
    }
}

DObjAnimMat *__cdecl CG_DObjCalcPose(const cpose_t *pose, const DObj_s *obj, int *partBits)
{
    DObjAnimMat *boneMatrix; // [esp+0h] [ebp-4h] BYREF

    iassert(obj);
    iassert(pose);

    if (!CL_DObjCreateSkelForBones(obj, partBits, &boneMatrix))
    {
        DObjCompleteHierarchyBits(obj, partBits);
        CG_DoControllers(pose, obj, partBits);
        DObjCalcSkel(obj, partBits);
    }

    return boneMatrix;
}


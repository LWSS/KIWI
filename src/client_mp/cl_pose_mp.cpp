#ifndef KISAK_MP
#error This File is MultiPlayer Only
#endif

#include <universal/q_shared.h>
#include "client_mp.h"

#include <gfx_d3d/r_scene.h>
#include <xanim/dobj_utils.h>

char *__cdecl CL_AllocSkelMemory(uint size)
{
    clientActive_t *skelGlob = &clients[R_GetLocalClientNum()];
    int skelMemPos;

    iassert(size);
    if (!size || size > 0x3FFF0)
        return NULL;
    size = (size + 16 - 1) & ~(16 - 1);
    iassert(skelGlob->skelMemoryStart);
    if (!skelGlob->skelMemoryStart)
        return NULL;

    skelMemPos = InterlockedExchangeAdd(&skelGlob->skelMemPos, size);
    if (skelMemPos < 0 || (uint)skelMemPos > 0x3FFF0 - size)
        return NULL;
    return skelGlob->skelMemoryStart + skelMemPos;
}

int __cdecl CL_GetSkelTimeStamp()
{
    return clients[R_GetLocalClientNum()].skelTimeStamp;
}

int warnCount_0;
int __cdecl CL_DObjCreateSkelForBones(const DObj_s *obj, int *partBits, DObjAnimMat **pMatOut)
{
    char *buf; // [esp+0h] [ebp-Ch]
    uint len; // [esp+4h] [ebp-8h]
    int timeStamp; // [esp+8h] [ebp-4h]

    iassert(obj);

    timeStamp = CL_GetSkelTimeStamp();
    if (DObjSkelExists(obj, timeStamp))
    {
        *pMatOut = I_dmaGetDObjSkel(obj);
        return DObjSkelAreBonesUpToDate(obj, partBits);
    }
    else
    {
        len = DObjGetAllocSkelSize(obj);
        buf = CL_AllocSkelMemory(len);
        if (buf)
        {
            *pMatOut = (DObjAnimMat *)buf;
            DObjCreateSkel((DObj_s*)obj, buf, timeStamp);
            return 0;
        }
        else
        {
            *pMatOut = 0;
            if (warnCount_0 != timeStamp)
            {
                warnCount_0 = timeStamp;
                Com_PrintWarning(CON_CHANNEL_CLIENT, "WARNING: CL_SKEL_MEMORY_SIZE exceeded - not calculating skeleton\n");
            }
            return 1;
        }
    }
}


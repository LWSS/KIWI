#include <universal/q_shared.h>
#include "r_shade.h"

uint __cdecl R_SkipDrawSurfListMaterial(const GfxDrawSurf *drawSurfList, uint drawSurfCount)
{
    uint subListCount = 0;

    while (subListCount < drawSurfCount && drawSurfList[subListCount].fields.materialSortedIndex == drawSurfList[0].fields.materialSortedIndex)
    {
        subListCount++;
    }

    return subListCount;
}
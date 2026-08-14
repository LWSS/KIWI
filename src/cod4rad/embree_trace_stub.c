/* Native fallbacks for builds that do not include the optional Embree bridge. */

#include "cod2rad64.h"

int g_useEmbree;

extern int rand_int(void);

void InitEmbreeScene(void)
{
    g_useEmbree = 0;
}

int TraceVisibility_Embree(float *startPos, float *endPos)
{
    return TraceVisibility(0, startPos, endPos);
}

int TraceStaticModels_Embree(float *startPos, float *endPos)
{
    return TraceStaticModels(startPos, endPos);
}

void TraceSetup_Embree(int cacheIndex, float *startPos, float *endPos, RayHitResult_t *hitResult)
{
    TraceSetup_and_Dispatch(cacheIndex, startPos, endPos, hitResult);
}

float ComputeAmbientOcclusion(float *pos, float *basis, int numSamples, float maxDist)
{
    float origin[3];
    float occluded = 0.0f;
    int i;

    if (numSamples <= 0 || maxDist <= 0.0f)
        return 1.0f;

    origin[0] = pos[0] + basis[6] * 0.1f;
    origin[1] = pos[1] + basis[7] * 0.1f;
    origin[2] = pos[2] + basis[8] * 0.1f;

    for (i = 0; i < numSamples; i++)
    {
        float u1 = (float)rand_int() / 32767.0f;
        float u2 = (float)rand_int() / 32767.0f;
        float radius = sqrtf(u1);
        float angle = 6.2831853f * u2;
        float localX = radius * cosf(angle);
        float localY = radius * sinf(angle);
        float localZ = sqrtf(1.0f - u1);
        float endPos[3];

        endPos[0] = origin[0] + maxDist * (localX * basis[0] + localY * basis[3] + localZ * basis[6]);
        endPos[1] = origin[1] + maxDist * (localX * basis[1] + localY * basis[4] + localZ * basis[7]);
        endPos[2] = origin[2] + maxDist * (localX * basis[2] + localY * basis[5] + localZ * basis[8]);

        if (!TraceVisibility(0, origin, endPos) || TraceStaticModels(origin, endPos))
            occluded += 1.0f;
    }

    return 1.0f - occluded / (float)numSamples;
}

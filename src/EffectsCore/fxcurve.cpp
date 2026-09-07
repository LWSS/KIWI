#include <universal/q_shared.h>
#include "fx_system.h"
#include <universal/com_memory.h>


double __cdecl FxCurve_Interpolate1d(const float *key, float intermediateTime)
{
    const char *v2; // eax
    float value1; // [esp+1Ch] [ebp-10h]
    float value0; // [esp+20h] [ebp-Ch]
    float time1; // [esp+24h] [ebp-8h]
    float time0; // [esp+28h] [ebp-4h]

    time0 = *key;
    time1 = key[2];
    value0 = key[1];
    value1 = key[3];
    if (intermediateTime < (double)time0 || time1 < (double)intermediateTime || time1 == time0)
    {
        v2 = va("%g, %g, %g", time0, time1, intermediateTime);
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\effectscore\\FxCurve.h",
            56,
            0,
            "%s\n\t%s",
            "time0 <= intermediateTime && intermediateTime <= time1 && time0 != time1",
            v2);
    }
    return (float)((intermediateTime - time0) * (value1 - value0) / (time1 - time0) + value0);
}

void __cdecl FxCurveIterator_Create(FxCurveIterator *createe, const FxCurve *master)
{
    iassert(createe);
    iassert(master);
    vassert((master->keyCount > 0), "(master->keyCount) = %i", master->keyCount);
    vassert((master->dimensionCount > 0), "(master->dimensionCount) = %i", master->dimensionCount);
    if (master == (const FxCurve *)-8)
        MyAssertHandler(".\\EffectsCore\\FxCurve.cpp", 67, 0, "%s", "master->keys");
    createe->master = master;
    createe->currentKeyIndex = 0;
}

void __cdecl FxCurveIterator_Release(FxCurveIterator *releasee)
{
    iassert(releasee);
    iassert(releasee->master);
    releasee->master = 0;
}

void __cdecl FxCurve_Interpolate3d(const float *key, float intermediateTime, float *result)
{
    const char *v3; // eax
    float fraction; // [esp+18h] [ebp-14h]
    float time1; // [esp+24h] [ebp-8h]
    float time0; // [esp+28h] [ebp-4h]

    time0 = *key;
    time1 = key[4];
    if (intermediateTime < (double)time0 || time1 < (double)intermediateTime || time1 == time0)
    {
        v3 = va("%g, %g, %g", time0, time1, intermediateTime);
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\effectscore\\FxCurve.h",
            74,
            0,
            "%s\n\t%s",
            "time0 <= intermediateTime && intermediateTime <= time1 && time0 != time1",
            v3);
    }
    fraction = (intermediateTime - time0) / (time1 - time0);
    Vec3Lerp(key + 1, key + 5, fraction, result);
}


const FxCurve *__cdecl FxCurve_AllocAndCreateWithKeys(float *keyArray, int dimensionCount, int keyCount)
{
    iassert(keyArray);
    if (dimensionCount <= 0 || dimensionCount == INT_MAX || keyCount <= 0 || keyCount > INT_MAX - 2)
        Com_Error(ERR_DROP, "Invalid effect curve dimensions");
    int keySize = dimensionCount + 1;
    if (keyCount > (INT_MAX - 2) / keySize)
        Com_Error(ERR_DROP, "Effect curve is too large");
    bool addStart = keyArray[0] != 0.0f;
    bool addEnd = keyArray[(keyCount - 1) * keySize] != 1.0f;
    int createdKeyCount = keyCount + addStart + addEnd;
    size_t bytes = offsetof(FxCurve, keys) + sizeof(float) * (size_t)createdKeyCount * keySize;
    if (createdKeyCount < 2 || bytes > INT_MAX)
        Com_Error(ERR_DROP, "Invalid effect curve size");
    FxCurve *curve = (FxCurve *)Hunk_AllocAlign((int)bytes, sizeof(float), "FxCurve_AllocAndCreateWithKeys", 8);
    curve->dimensionCount = dimensionCount;
    curve->keyCount = createdKeyCount;
    if (addStart)
    {
        memcpy(curve->keys, keyArray, sizeof(float) * keySize);
        curve->keys[0] = 0.0f;
    }
    memcpy(curve->keys + addStart * keySize, keyArray, sizeof(float) * keySize * keyCount);
    if (addEnd)
    {
        float *last = curve->keys + (createdKeyCount - 1) * keySize;
        memcpy(last, keyArray + (keyCount - 1) * keySize, sizeof(float) * keySize);
        last[0] = 1.0f;
    }
    return curve;
}


void __cdecl FxCurveIterator_SampleTimeVec3(FxCurveIterator *source, float *replyVector, float time)
{
    FxCurveIterator_MoveToTime(source, time);
    bcassert(source->currentKeyIndex, (uint)(source->master->keyCount - 1));
    FxCurve_Interpolate3d(&source->master->keys[4 * source->currentKeyIndex], time, replyVector);
}

double __cdecl FxCurveIterator_SampleTime(FxCurveIterator *source, float time)
{
    FxCurveIterator_MoveToTime(source, time);
    bcassert(source->currentKeyIndex, (uint)(source->master->keyCount - 1));
    return (float)FxCurve_Interpolate1d(&source->master->keys[2 * source->currentKeyIndex], time);
}

void __cdecl FxCurveIterator_MoveToTime(FxCurveIterator *source, float time)
{
    const char *v2; // eax
    int keySize; // [esp+8h] [ebp-8h]
    const float *key; // [esp+Ch] [ebp-4h]

    iassert(source);
    iassert(source->master);
    if (source->master == (const FxCurve *)-8)
        MyAssertHandler("c:\\trees\\cod3\\src\\effectscore\\FxCurve.h", 87, 0, "%s", "source->master->keys");
    vassert((time >= 0.0f && time <= 1.0f), "(time) = %g", time);
    vassert((source->master->keyCount > 0), "(source->master->keyCount) = %i", source->master->keyCount);
    vassert((source->master->dimensionCount > 0), "(source->master->dimensionCount) = %i", source->master->dimensionCount);
    bcassert(source->currentKeyIndex, (uint)source->master->keyCount);
    keySize = source->master->dimensionCount + 1;
    key = &source->master->keys[keySize * source->currentKeyIndex];
    if (*key > (double)time)
    {
        source->currentKeyIndex = 0;
        key = source->master->keys;
    }
    while (key[keySize] < (double)time)
    {
        ++source->currentKeyIndex;
        key += keySize;
    }
    if (key != &source->master->keys[keySize * source->currentKeyIndex])
    {
        v2 = va("%p != %p", key, &source->master->keys[keySize * source->currentKeyIndex]);
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\effectscore\\FxCurve.h",
            108,
            1,
            "%s\n\t%s",
            "key == &source->master->keys[source->currentKeyIndex * keySize]",
            v2);
    }
    if (source->currentKeyIndex >= (uint)source->master->keyCount)
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\effectscore\\FxCurve.h",
            109,
            1,
            "source->currentKeyIndex doesn't index source->master->keyCount\n\t%i not in [0, %i)",
            source->currentKeyIndex,
            source->master->keyCount);
}
#pragma once
#include "r_rendercmds.h"

enum GfxLightType : int
{
    GFX_LIGHT_TYPE_NONE = 0x0,
    GFX_LIGHT_TYPE_DIR = 0x1,
    GFX_LIGHT_TYPE_SPOT = 0x2,
    GFX_LIGHT_TYPE_OMNI = 0x3,
    GFX_LIGHT_TYPE_COUNT = 0x4,
    GFX_LIGHT_TYPE_DIR_SHADOWMAP = 0x4,
    GFX_LIGHT_TYPE_SPOT_SHADOWMAP = 0x5,
    GFX_LIGHT_TYPE_OMNI_SHADOWMAP = 0x6,
    GFX_LIGHT_TYPE_COUNT_WITH_SHADOWMAP_VERSIONS = 0x7,
};

struct GfxCandidateShadowedLight // sizeof=0x8
{                                       // ...
    uint shadowableLightIndex;  // ...
    float score;
};

struct GfxShadowedLightEntry // sizeof=0x8
{                                       // ...
    uint8_t shadowableLightIndex;
    bool isFadingOut;
    // padding byte
    // padding byte
    float fade;
};
struct GfxShadowedLightHistory // sizeof=0x48
{                                       // ...
    uint shadowableLightWasUsed[8];
    GfxShadowedLightEntry entries[4];
    uint entryCount;
    uint lastUpdateTime;
};

struct GfxShadowGeometry // sizeof=0xC
{
    uint16_t surfaceCount;
    uint16_t smodelCount;
    uint16_t *sortedSurfIndex;
    uint16_t *smodelIndex;
};
struct GfxLightRegionAxis // sizeof=0x14
{
    float dir[3];
    float midPoint;
    float halfSize;
};
struct GfxLightRegionHull // sizeof=0x50
{
    float kdopMidPoint[9];
    float kdopHalfSize[9];
    uint axisCount;
    GfxLightRegionAxis *axis;
};
struct GfxLightRegion // sizeof=0x8
{
    uint hullCount;
    GfxLightRegionHull *hulls;
};

void __cdecl R_ClearShadowedPrimaryLightHistory(int localClientNum);
void __cdecl R_AddDynamicShadowableLight(GfxViewInfo *viewInfo, const GfxLight *visibleLight);
bool __cdecl R_IsDynamicShadowedLight(uint shadowableLightIndex);
bool __cdecl R_IsPrimaryLight(uint shadowableLightIndex);
void __cdecl R_ChooseShadowedLights(GfxViewInfo *viewInfo);
uint __cdecl R_AddPotentiallyShadowedLight(
    const GfxViewInfo *viewInfo,
    uint shadowableLightIndex,
    GfxCandidateShadowedLight *candidateLights,
    uint candidateLightCount);
double __cdecl R_ShadowedSpotLightScore(const GfxViewParms *viewParms, const GfxLight *light);
void __cdecl R_AddShadowsForLight(GfxViewInfo *viewInfo, uint shadowableLightIndex, float spotShadowFade);
void __cdecl R_AddShadowedLightToShadowHistory(
    GfxShadowedLightHistory *shadowHistory,
    uint shadowableLightIndex,
    float fadeDelta);
void __cdecl R_FadeOutShadowHistoryEntries(GfxShadowedLightHistory *shadowHistory, float fadeDelta);
void __cdecl R_LinkSphereEntityToPrimaryLights(
    uint localClientNum,
    uint entityNum,
    const float *origin,
    float radius);
uint __cdecl R_GetPrimaryLightEntityShadowBit(
    uint localClientNum,
    uint entnum,
    uint primaryLightIndex);
void __cdecl R_LinkBoxEntityToPrimaryLights(
    uint localClientNum,
    uint entityNum,
    const float *mins,
    const float *maxs);
char __cdecl R_CullBoxFromLightRegionHull(
    const GfxLightRegionHull *hull,
    const float *boxMidPoint,
    const float *boxHalfSize);
void __cdecl R_LinkDynEntToPrimaryLights(
    uint dynEntId,
    DynEntityDrawType drawType,
    const float *mins,
    const float *maxs);
bool __cdecl Com_CullBoxFromPrimaryLight(
    const struct ComPrimaryLight *light,
    const float *boxMidPoint,
    const float *boxHalfSize);
uint __cdecl R_GetPrimaryLightDynEntShadowBit(uint entnum, uint primaryLightIndex);
void __cdecl R_UnlinkEntityFromPrimaryLights(uint localClientNum, uint entityNum);
void __cdecl R_UnlinkDynEntFromPrimaryLights(uint dynEntId, DynEntityDrawType drawType);
bool __cdecl R_IsEntityVisibleToPrimaryLight(
    uint localClientNum,
    uint entityNum,
    uint primaryLightIndex);
bool __cdecl R_IsDynEntVisibleToPrimaryLight(
    uint dynEntId,
    DynEntityDrawType drawType,
    uint primaryLightIndex);
int __cdecl R_IsEntityVisibleToAnyShadowedPrimaryLight(const GfxViewInfo *viewInfo, uint entityNum);
bool __cdecl R_IsEntityVisibleToShadowedPrimaryLight(uint baseBitIndex, uint shadowableLightIndex);
int __cdecl R_IsDynEntVisibleToAnyShadowedPrimaryLight(
    const GfxViewInfo *viewInfo,
    uint dynEntId,
    DynEntityDrawType drawType);
bool __cdecl R_IsDynEntVisibleToShadowedPrimaryLight(
    uint baseBitIndex,
    DynEntityDrawType drawType,
    uint shadowableLightIndex);
uint __cdecl R_GetNonSunPrimaryLightForBox(
    const GfxViewInfo *viewInfo,
    const float *boxMidPoint,
    const float *boxHalfSize);
uint __cdecl R_GetNonSunPrimaryLightForSphere(const GfxViewInfo *viewInfo, const float *origin, float radius);
char __cdecl R_CullSphereFromLightRegionHull(const GfxLightRegionHull *hull, const float *origin, float radius);
bool __cdecl Com_CullSphereFromPrimaryLight(const struct ComPrimaryLight *light, const float *origin, float radius);

#pragma once
#include "r_gfx.h"
#include "r_rendercmds.h"
#include "rb_backend.h"

enum GfxModelLightExtrapolation : int
{                                       // ...
    GFX_MODELLIGHT_EXTRAPOLATE = 0x0,
    GFX_MODELLIGHT_SHOW_MISSING = 0x1,
};

struct GfxLightingInfo // sizeof=0x2
{                                       // ...
    uint8_t primaryLightIndex;  // ...
    uint8_t reflectionProbeIndex; // ...
};


void __cdecl R_SetModelLightingCoords(uint16_t handle, float *out);
uint __cdecl R_ModelLightingIndexFromHandle(uint16_t handle);
void __cdecl R_GetPackedStaticModelLightingCoords(uint smodelIndex, PackedLightingCoords *packedCoords);
char __cdecl R_AllocStaticModelLighting(GfxStaticModelDrawInst *smodelDrawInst, uint smodelIndex);
uint __cdecl R_AllocModelLighting_PrimaryLight(
    float *lightingOrigin,
    uint dynEntId,
    uint16_t *cachedLightingHandle,
    GfxLightingInfo *lightingInfoOut);
uint __cdecl R_AllocModelLighting(
    float *lightingOrigin,
    uint16_t *cachedLightingHandle,
    uint(__cdecl *GetPrimaryLightCallback)(const void *),
    const void *userData,
    GfxLightingInfo *lightingInfoOut);
uint __cdecl R_DynEntPrimaryLightCallback(const void *userData);
uint __cdecl R_AllocModelLighting_Box(
    const GfxViewInfo *viewInfo,
    float *lightingOrigin,
    const float *boxMins,
    const float *boxMaxs,
    uint16_t *cachedLightingHandle,
    GfxLightingInfo *lightingInfoOut);
uint __cdecl R_GetPrimaryLightForBoxCallback(const void *userData);
uint __cdecl R_AllocModelLighting_Sphere(
    const GfxViewInfo *viewInfo,
    float *lightingOrigin,
    const float *origin,
    float radius,
    uint16_t *cachedLightingHandle,
    GfxLightingInfo *lightingInfoOut);
uint __cdecl R_GetPrimaryLightForSphereCallback(const void *userData);
void __cdecl R_ToggleModelLightingFrame();
uint __cdecl R_CalcModelLighting(
    uint entryIndex,
    const float *lightingOrigin,
    uint nonSunPrimaryLightIndex,
    GfxModelLightExtrapolation extrapolateBehavior);
void __cdecl R_BeginAllStaticModelLighting();
void __cdecl R_SetAllStaticModelLighting();
void __cdecl R_SetStaticModelLighting(uint smodelIndex);
void __cdecl R_SetModelGroundLighting(uint entryIndex, const uint8_t *groundLighting);
void __cdecl R_SetModelLightingCoordsForSource(uint16_t handle, GfxCmdBufSourceState *source);
void __cdecl R_SetStaticModelLightingCoordsForSource(uint smodelIndex, GfxCmdBufSourceState *source);
uint R_SetModelLightingSampleDeltas();
void __cdecl R_SetModelLightingLookupScale(GfxCmdBufInput *input);
void __cdecl R_SetupDynamicModelLighting(GfxCmdBufInput *input);
void __cdecl R_InitModelLightingGlobals();
void __cdecl R_ShutdownModelLightingGlobals();
char *__cdecl R_AllocModelLightingGlobal(uint bytes);
void __cdecl R_ResetModelLighting();
void __cdecl R_InitModelLightingImage();
void __cdecl R_ShutdownModelLightingImage();
void __cdecl R_InitStaticModelLighting();

void __cdecl RB_PatchModelLighting(const GfxModelLightingPatch *patchList, uint patchCount);
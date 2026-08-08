#pragma once
#include "rb_backend.h"
#include "rb_tess.h"

struct GfxStaticModelDrawStream // sizeof=0x1C
{                                       // ...
    const uint *primDrawSurfPos; // ...
    const GfxTexture *reflectionProbeTexture; // ...
    uint customSamplerFlags;    // ...
    XSurface *localSurf;
    uint smodelCount;
    const uint16_t *smodelList;
    uint reflectionProbeIndex;
};

void __cdecl R_DrawStaticModelSurfLit(const uint *primDrawSurfPos, GfxCmdBufContext context);
int __cdecl R_GetNextStaticModelSurf(GfxStaticModelDrawStream *drawStream, XSurface **outSurf);
void __cdecl R_DrawStaticModelSurf(const uint *primDrawSurfPos, GfxCmdBufContext context);
void __cdecl R_DrawStaticModelDrawSurfNonOptimized(GfxStaticModelDrawStream *drawStream, GfxCmdBufContext context);
void __cdecl R_SetStaticModelVertexBuffer(GfxCmdBufPrimState *primState, XSurface *xsurf);
void __cdecl R_DrawStaticModelDrawSurfPlacement(
    const GfxStaticModelDrawInst *smodelDrawInst,
    GfxCmdBufSourceState *source);
void __cdecl R_DrawStaticModelDrawSurfLightingNonOptimized(
    GfxStaticModelDrawStream *drawStream,
    GfxCmdBufContext context);

void __cdecl R_DrawStaticModelCachedSurfLit(const uint *primDrawSurfPos, GfxCmdBufContext context);
void __cdecl R_DrawStaticModelCachedSurf(const uint *primDrawSurfPos, GfxCmdBufContext context);
void __cdecl R_SetupCachedStaticModelLighting(GfxCmdBufSourceState *source);
int __cdecl R_ReadStaticModelPreTessDrawSurf(
    GfxReadCmdBuf *readCmdBuf,
    GfxStaticModelPreTessSurf *pretessSurf,
    uint *firstIndex,
    uint *count);
void __cdecl R_DrawStaticModelsPreTessDrawSurf(
    GfxStaticModelPreTessSurf pretessSurf,
    uint firstIndex,
    uint count,
    GfxCmdBufContext context);
void __cdecl R_DrawStaticModelsPreTessDrawSurfLighting(
    GfxStaticModelPreTessSurf pretessSurf,
    uint firstIndex,
    uint count,
    GfxCmdBufContext context);

void __cdecl R_DrawStaticModelSkinnedSurf(const uint *primDrawSurfPos, GfxCmdBufContext context);
void __cdecl R_DrawStaticModelSkinnedSurfLit(const uint *primDrawSurfPos, GfxCmdBufContext context);
void __cdecl R_DrawStaticModelsSkinnedDrawSurf(GfxStaticModelDrawStream *drawStream, GfxCmdBufContext context);

uint __cdecl R_ReadPrimDrawSurfInt(GfxReadCmdBuf *cmdBuf);
const uint *__cdecl R_ReadPrimDrawSurfData(GfxReadCmdBuf *cmdBuf, uint count);
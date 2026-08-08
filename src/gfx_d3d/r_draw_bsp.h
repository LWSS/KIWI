#pragma once
#include "rb_backend.h"

struct GfxTrianglesDrawStream // sizeof=0x30
{                                       // ...
    uint reflectionProbeCount;  // ...
    uint lightmapCount;         // ...
    GfxTexture *reflectionProbeTextures; // ...
    GfxTexture *lightmapPrimaryTextures; // ...
    GfxTexture *lightmapSecondaryTextures; // ...
    GfxTexture *whiteTexture;           // ...
    const uint *primDrawSurfPos; // ...
    const GfxTexture *reflectionProbeTexture; // ...
    const GfxTexture *lightmapPrimaryTexture; // ...
    const GfxTexture *lightmapSecondaryTexture; // ...
    uint customSamplerFlags;    // ...
    int hasSunDirChanged;               // ...
};

void __cdecl R_SetStreamSource(
    GfxCmdBufPrimState *primState,
    IDirect3DVertexBuffer9 *vb,
    uint vertexOffset,
    uint vertexStride);
void __cdecl R_HW_SetSamplerTexture(IDirect3DDevice9 *device, uint samplerIndex, const GfxTexture *texture);
void __cdecl R_SetStreamsForBspSurface(GfxCmdBufPrimState *state, const srfTriangles_t *tris);
void __cdecl R_DrawBspDrawSurfsLit(
    const uint *primDrawSurfPos,
    GfxCmdBufContext context,
    GfxCmdBufContext prepassContext);
void __cdecl R_DrawTrianglesLit(
    GfxTrianglesDrawStream *drawStream,
    GfxCmdBufPrimState *primState,
    GfxCmdBufPrimState *prepassPrimState);
void __cdecl R_DrawBspTris(GfxCmdBufPrimState *state, const srfTriangles_t *tris, uint triCount);
int __cdecl R_ReadBspDrawSurfs(
    const uint **primDrawSurfPos,
    const uint16_t **list,
    uint *count);
void __cdecl R_DrawBspDrawSurfs(const uint *primDrawSurfPos, GfxCmdBufState *state);
void __cdecl R_DrawTriangles(GfxTrianglesDrawStream *drawStream, GfxCmdBufPrimState *state);

void __cdecl R_DrawPreTessTris(
    GfxCmdBufPrimState *state,
    const srfTriangles_t *tris,
    uint baseIndex,
    uint triCount);

void __cdecl R_DrawBspDrawSurfsPreTess(const uint *primDrawSurfPos, GfxCmdBufContext context);
void __cdecl R_DrawBspDrawSurfsLitPreTess(const uint *primDrawSurfPos, GfxCmdBufContext context);
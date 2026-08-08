#pragma once
#include "rb_backend.h"


void __cdecl R_DrawXModelRigidModelSurf(GfxCmdBufContext context, XSurface *xsurf);

uint __cdecl R_DrawXModelRigidSurf(
    const GfxDrawSurf *drawSurfList,
    uint drawSurfCount,
    GfxCmdBufContext context);

uint __cdecl R_DrawXModelRigidSurfCamera(
    const GfxDrawSurf *drawSurfList,
    uint drawSurfCount,
    GfxCmdBufContext context);

uint __cdecl R_DrawXModelRigidSurfLit(
    const GfxDrawSurf *drawSurfList,
    uint drawSurfCount,
    GfxCmdBufContext context);
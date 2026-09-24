#pragma once
#include "r_rendercmds.h"

void __cdecl TRACK_rb_sunshadow();
void __cdecl RB_SunShadowMaps(const GfxBackEndData *data, const GfxViewInfo *viewInfo);
void __cdecl RB_DrawSunShadowOverlay();
void __cdecl RB_GetShadowOverlayDepthBounds(float *nearDepth, float *farDepth); // used in rb_spotshadow??

// KIWI: sm_showOverlay on PC hardware shadow maps (see rb_sunshadow.cpp).
MaterialPixelShader *RB_BeginShadowOverlayShader();
void RB_EndShadowOverlayShader(MaterialPixelShader *stockShader);
void RB_ReleaseShadowOverlayShader();
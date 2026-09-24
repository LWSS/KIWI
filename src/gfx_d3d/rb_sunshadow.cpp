#include <universal/q_shared.h>
#include "rb_sunshadow.h"
#include <qcommon/mem_track.h>
#include "r_dvars.h"
#include "rb_backend.h"
#include "r_draw_sunshadow.h"
#include "r_cmdbuf.h"
#include "rb_pixelcost.h"
#include "rb_state.h"
#include "rb_shade.h"
#include "r_sunshadow.h"
#include "rb_postfx.h"
#include <universal/profile.h>
#include "r_state.h"
#include "r_material.h"
#include "r_init.h"
#include <universal/com_memory.h>
#include <d3dx9shader.h>

GfxPointVertex g_overlayPoints[36];

void __cdecl TRACK_rb_sunshadow()
{
    track_static_alloc_internal(g_overlayPoints, 576, "g_overlayPoints", 18);
}

void __cdecl RB_SunShadowMaps(const GfxBackEndData *data, const GfxViewInfo *viewInfo)
{
    GfxCmdBuf cmdBuf; // [esp+0h] [ebp-8h] BYREF

    if (pixelCostMode == GFX_PIXEL_COST_MODE_OFF)
    {
        iassert(data);

        R_InitContext(data, &cmdBuf);

        PROF_SCOPED("Sun Shadow Maps");

        for (int partitionIndex = 0; partitionIndex < 2; ++partitionIndex)
        {
            ZoneTextF("Sun Shadow Map %d", partitionIndex);
            R_DrawSunShadowMap(viewInfo, partitionIndex, &cmdBuf);
        }
    }
}

void __cdecl RB_GetShadowOverlayDepthBounds(float *nearDepth, float *farDepth)
{
    *nearDepth = sm_showOverlayDepthBounds->current.vector[0];
    *farDepth = sm_showOverlayDepthBounds->current.vector[1];

    if (I_fabs(*farDepth - *nearDepth) < 0.01f)
    {
        if (*farDepth <= *nearDepth)
        {
            *farDepth = (*nearDepth + *farDepth) * 0.5 - 0.005f;
            *nearDepth = *farDepth + 0.01f;
        }
        else
        {
            *nearDepth = (*nearDepth + *farDepth) * 0.5 - 0.005f;
            *farDepth = *nearDepth + 0.01f;
        }
    }
}


// KIWI: sm_showOverlay has never worked on PC.  shadowmap_display.hlsl reads the shadow map
// texel as a depth, which is what the 360 gets from its resolved float-depth target.  On PC
// with hardware shadow maps (gfxMetrics.hasHardwareShadowmap) the target is a D24S8 depth
// texture, and sampling one returns the depth COMPARISON against texcoord.z, not the depth,
// so the overlay came out one flat colour.  For the overlay draw only, the pass gets this
// shader instead: it recovers the stored depth by bisecting on the comparison, then does
// exactly what shadowmap_display does (same s0 / filterTap c12 registers, so the pass's
// arguments bind unchanged).  The R32F colour path is untouched.
static const char s_shadowOverlayDepthHlsl[] =
    "sampler2D colorMapSampler : register(s0);\n"
    "float4 filterTap0 : register(c12);\n"
    "float Probe(float2 uv, float ref) { return tex2Dproj(colorMapSampler, float4(uv, ref, 1.0)).x; }\n"
    "float4 ps_main(float2 uv : TEXCOORD0) : COLOR\n"
    "{\n"
    "    float nearAnswer = Probe(uv, 0.0);\n"
    "    float lo = 0.0;\n"
    "    float hi = 1.0;\n"
    "    [unroll] for (int i = 0; i < 16; ++i)\n"
    "    {\n"
    "        float mid = (lo + hi) * 0.5;\n"
    "        float same = step(abs(Probe(uv, mid) - nearAnswer), 0.5);\n"
    "        lo = lerp(lo, mid, same);\n"
    "        hi = lerp(mid, hi, same);\n"
    "    }\n"
    "    float depth = lerp((lo + hi) * 0.5, 1.0, step(abs(Probe(uv, 1.0) - nearAnswer), 0.5));\n"
    "    float linearDepth = depth * filterTap0.z / lerp(filterTap0.w, filterTap0.z, depth);\n"
    "    float grey = saturate(linearDepth * filterTap0.x + filterTap0.y);\n"
    "    return depth == 1.0 ? float4(0.0, 0.0, 0.5, 1.0) : float4(grey, grey, grey, 1.0);\n"
    "}\n";

static MaterialPixelShader s_shadowOverlayDepthShader;
static bool s_shadowOverlayDepthShaderFailed;

static bool RB_CreateShadowOverlayDepthShader(const MaterialPixelShader *stockShader)
{
    ID3DXBuffer *program = nullptr;
    ID3DXBuffer *errors = nullptr;
    HRESULT hr = D3DXCompileShader(
        s_shadowOverlayDepthHlsl, sizeof(s_shadowOverlayDepthHlsl) - 1, nullptr, nullptr, "ps_main", "ps_3_0", 0, &program, &errors, nullptr);
    if (FAILED(hr))
    {
        Com_PrintWarning(CON_CHANNEL_GFX, "sm_showOverlay: depth overlay shader failed to compile: %s\n",
                         errors ? (const char *)errors->GetBufferPointer() : R_ErrorDescription(hr));
        if (errors)
            errors->Release();
        return false;
    }
    if (errors)
        errors->Release();

    // Kept for the lifetime of the shader: Material_GetTechnique checks loadDef.loadForRenderer.
    const uint programSize = program->GetBufferSize();
    void *programCopy = Z_Malloc(programSize, "RB_CreateShadowOverlayDepthShader", 0);
    memcpy(programCopy, program->GetBufferPointer(), programSize);
    program->Release();

    hr = dx.device->CreatePixelShader((const DWORD *)programCopy, &s_shadowOverlayDepthShader.prog.ps);
    if (FAILED(hr))
    {
        Com_PrintWarning(CON_CHANNEL_GFX, "sm_showOverlay: depth overlay shader creation failed: %s\n", R_ErrorDescription(hr));
        Z_Free(programCopy, 0);
        return false;
    }
    s_shadowOverlayDepthShader.name = "kiwi_shadowmap_display_depth";
    s_shadowOverlayDepthShader.prog.loadDef.program = programCopy;
    s_shadowOverlayDepthShader.prog.loadDef.programSize = (uint16_t)(programSize >> 2);
    s_shadowOverlayDepthShader.prog.loadDef.loadForRenderer = stockShader->prog.loadDef.loadForRenderer;
    return true;
}

// Swaps the overlay pass's pixel shader when the shadow map is a hardware depth texture.
// Returns the stock shader to hand back to RB_EndShadowOverlayShader, or NULL if nothing
// was swapped.  Every draw between the two must be flushed (RB_EndTessSurface) before End.
MaterialPixelShader *RB_BeginShadowOverlayShader()
{
    if (!gfxMetrics.hasHardwareShadowmap || s_shadowOverlayDepthShaderFailed)
        return nullptr;

    // The asset is shared, not const; the pointer is restored right after the overlay draw.
    MaterialPass *pass = (MaterialPass *)&Material_GetTechnique(rgp.shadowOverlayMaterial, TECHNIQUE_UNLIT)->passArray[0];
    MaterialPixelShader *stockShader = pass->pixelShader;
    if (!stockShader || !stockShader->prog.ps)
        return nullptr;

    if (!s_shadowOverlayDepthShader.prog.ps)
    {
        // ps_3_0 has to pair with the pass's vs_3_0; the Shader Model 2 renderer keeps the stock shader.
        DWORD version = 0;
        UINT size = sizeof(version);
        if (stockShader->prog.ps->GetFunction(nullptr, &size) != D3D_OK || size < sizeof(version))
            return nullptr;
        void *function = Z_Malloc(size, "RB_BeginShadowOverlayShader", 0);
        stockShader->prog.ps->GetFunction(function, &size);
        version = *(const DWORD *)function;
        Z_Free(function, 0);
        if (D3DSHADER_VERSION_MAJOR(version) < 3 || !RB_CreateShadowOverlayDepthShader(stockShader))
        {
            s_shadowOverlayDepthShaderFailed = true;
            return nullptr;
        }
    }

    pass->pixelShader = &s_shadowOverlayDepthShader;
    return stockShader;
}

void RB_EndShadowOverlayShader(MaterialPixelShader *stockShader)
{
    if (!stockShader)
        return;
    MaterialPass *pass = (MaterialPass *)&Material_GetTechnique(rgp.shadowOverlayMaterial, TECHNIQUE_UNLIT)->passArray[0];
    iassert(pass->pixelShader == &s_shadowOverlayDepthShader);
    pass->pixelShader = stockShader;
}

// Pixel shaders survive a device Reset; this only runs from R_ShutdownDirect3D.
void RB_ReleaseShadowOverlayShader()
{
    if (s_shadowOverlayDepthShader.prog.ps)
    {
        s_shadowOverlayDepthShader.prog.ps->Release();
        s_shadowOverlayDepthShader.prog.ps = nullptr;
    }
    if (s_shadowOverlayDepthShader.prog.loadDef.program)
    {
        Z_Free(s_shadowOverlayDepthShader.prog.loadDef.program, 0);
        s_shadowOverlayDepthShader.prog.loadDef.program = nullptr;
    }
    s_shadowOverlayDepthShaderFailed = false;
}

static void __cdecl RB_SunShadowOverlayPoint(const float *xy, float x0, float y0, float w, float h, float *point)
{
    point[0] = ((xy[0] * 0.5f) + 0.5f) * w + x0;
    point[1] = (0.5f - (xy[1] * 0.5f)) * h + y0;
    point[2] = 0.0f;
}

static void RB_SetSunShadowOverlayScaleAndBias()
{
    float nearDepth; // [esp+18h] [ebp-10h] BYREF
    float bias; // [esp+1Ch] [ebp-Ch]
    float scale; // [esp+20h] [ebp-8h]
    float farDepth; // [esp+24h] [ebp-4h] BYREF

    RB_GetShadowOverlayDepthBounds(&nearDepth, &farDepth);
    scale = 1.0f / (farDepth - nearDepth);
    bias = -scale * nearDepth;
    R_UpdateCodeConstant(&gfxCmdBufSourceState, CONST_SRC_CODE_FILTER_TAP_0, scale, bias, 1.0f, 1.0f);
}

void __cdecl RB_DrawSunShadowOverlay()
{
    float v0; // [esp+28h] [ebp-C4h]
    float t0; // [esp+3Ch] [ebp-B0h]
    float x0; // [esp+44h] [ebp-A8h]
    float t1; // [esp+48h] [ebp-A4h]
    float clipSpacePoints[9][2]; // [esp+4Ch] [ebp-A0h] BYREF
    int pointIsNear[9]; // [esp+98h] [ebp-54h] BYREF
    float shadowSampleSize; // [esp+BCh] [ebp-30h]
    const GfxViewInfo *viewInfo; // [esp+C0h] [ebp-2Ch]
    int pointIndexDst; // [esp+C4h] [ebp-28h]
    int pointIndexSrc; // [esp+C8h] [ebp-24h]
    int partitionIndex; // [esp+CCh] [ebp-20h]
    float y0; // [esp+D0h] [ebp-1Ch]
    float x; // [esp+D4h] [ebp-18h]
    float y; // [esp+D8h] [ebp-14h]
    float h; // [esp+DCh] [ebp-10h]
    const GfxSunShadow *sunShadow; // [esp+E0h] [ebp-Ch]
    const GfxSunShadowPartition *partition; // [esp+E4h] [ebp-8h]
    float w; // [esp+E8h] [ebp-4h]

    iassert(backEndData->viewInfoCount > 0);
    viewInfo = backEndData->viewInfo;
    sunShadow = &viewInfo->sunShadow;
    x0 = 4.0f;
    y0 = 4.0f;
    h = (float)vidConfig.displayHeight * 0.5f;
    w = h;
    RB_SetSunShadowOverlayScaleAndBias();
    gfxCmdBufSourceState.input.codeImageSamplerStates[TEXTURE_SRC_CODE_FEEDBACK] = (SAMPLER_CLAMP_V | SAMPLER_CLAMP_U | SAMPLER_FILTER_NEAREST);
    R_SetCodeImageTexture(&gfxCmdBufSourceState, TEXTURE_SRC_CODE_FEEDBACK, gfxRenderTargets[R_RENDERTARGET_SHADOWMAP_SUN].image);
    MaterialPixelShader *stockShader = RB_BeginShadowOverlayShader(); // KIWI
    for (partitionIndex = 0; partitionIndex < 2; ++partitionIndex)
    {
        t0 = (float)partitionIndex * 0.5f;
        t1 = t0 + 0.5f;
        v0 = (float)partitionIndex * w + x0;
        RB_DrawStretchPic(rgp.shadowOverlayMaterial, v0, y0, w, h, 0.0f, t0, 1.0f, t1, 0xFFFFFFFF, GFX_PRIM_STATS_HUD);
    }
    RB_EndTessSurface();
    RB_EndShadowOverlayShader(stockShader); // KIWI
    gfxCmdBufSourceState.input.codeImageSamplerStates[TEXTURE_SRC_CODE_FEEDBACK] = (SAMPLER_CLAMP_V | SAMPLER_CLAMP_U | SAMPLER_FILTER_LINEAR);
    shadowSampleSize = sm_sunSampleSizeNear->current.value;
    pointIndexDst = 0;
    for (partitionIndex = 0; partitionIndex < 2; ++partitionIndex)
    {
        x = (double)partitionIndex * w + x0;
        y = y0;
        partition = &sunShadow->partition[partitionIndex];
        R_SunShadowMapBoundingPoly(
            &sunShadow->partition[partitionIndex].boundingPoly,
            shadowSampleSize,
            &clipSpacePoints,
            pointIsNear);
        for (pointIndexSrc = 0; pointIndexSrc < partition->boundingPoly.pointCount; ++pointIndexSrc)
        {
            if (!partitionIndex
                || !rg.sunShadowFull
                || pointIsNear[pointIndexSrc]
                || pointIsNear[(pointIndexSrc + 1) % partition->boundingPoly.pointCount])
            {
                RB_SunShadowOverlayPoint(clipSpacePoints[pointIndexSrc], x, y, w, h, g_overlayPoints[pointIndexDst].xyz);
                RB_SunShadowOverlayPoint(
                    clipSpacePoints[(pointIndexSrc + 1) % partition->boundingPoly.pointCount],
                    x,
                    y,
                    w,
                    h,
                    g_overlayPoints[pointIndexDst + 1].xyz);
                *(uint *)g_overlayPoints[pointIndexDst].color = -16711936;
                *(uint *)g_overlayPoints[pointIndexDst + 1].color = -16711936;
                pointIndexDst += 2;
            }
        }
        shadowSampleSize = shadowSampleSize * rg.sunShadowPartitionRatio;
    }
    if (pointIndexDst)
    {
        RB_DrawLines2D(pointIndexDst / 2, 1, g_overlayPoints);
        RB_EndTessSurface();
    }
}



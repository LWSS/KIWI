#pragma once

#include <qcommon/qcommon.h>

#include "r_debug.h"
#include "r_font.h"
#include "r_gfx.h"
#include "r_material.h"
#include <qcommon/com_pack.h>

enum CodeConstant : int; // r_state.h

enum GfxRenderCommand : int
{                                       // ...
    RC_END_OF_LIST = 0x0,
    RC_SET_MATERIAL_COLOR = 0x1,
    RC_SAVE_SCREEN = 0x2,
    RC_SAVE_SCREEN_SECTION = 0x3,
    RC_CLEAR_SCREEN = 0x4,
    RC_SET_VIEWPORT = 0x5,
    RC_FIRST_NONCRITICAL = 0x6,
    RC_STRETCH_PIC = 0x6,
    RC_STRETCH_PIC_FLIP_ST = 0x7,
    RC_STRETCH_PIC_ROTATE_XY = 0x8,
    RC_STRETCH_PIC_ROTATE_ST = 0x9,
    RC_STRETCH_RAW = 0xA,
    RC_DRAW_QUAD_PIC = 0xB,
    RC_DRAW_FULL_SCREEN_COLORED_QUAD = 0xC,
    RC_DRAW_TEXT_2D = 0xD,
    RC_DRAW_TEXT_3D = 0xE,
    RC_BLEND_SAVED_SCREEN_BLURRED = 0xF,
    RC_BLEND_SAVED_SCREEN_FLASHED = 0x10,
    RC_DRAW_POINTS = 0x11,
    RC_DRAW_LINES = 0x12,
    RC_DRAW_TRIANGLES = 0x13,
    RC_DRAW_PROFILE = 0x14,
    RC_PROJECTION_SET = 0x15,
#ifdef KISAK_RADIANT
    // Editor begin-view command (CoD4Radiant r_rendercmds.cpp). kisak's CoD3
    // renderer has no such command — the editor draws lines/text directly in the
    // command list (outside a scene render), so it needs a command to establish
    // the active GfxViewParms (which RB_DrawLines3D reads via viewParms3D). Appended
    // after RC_PROJECTION_SET so existing enum values are untouched; RC_COUNT bumps
    // so R_GetCommandBuffer's `renderCmd < RC_COUNT` guard admits it.
    RC_BEGIN_VIEW = 0x16,
    // Editor surface-cache flush (CoD4Radiant r_ed_scene.cpp). Emitted by
    // R_AddEditorSurfsCmd; the backend handler (RB_DrawEditorSkinnedCachedCmd) draws
    // the accumulated brush/world meshes from the per-material editor VB pool. Lighting
    // epic Stage 1 (2026-06-12). Appended after RC_BEGIN_VIEW so existing values hold.
    RC_DRAW_EDITOR_SKINNEDCACHED = 0x17,
    // Sun-light-preview custom shader constant (#26 layer C2, CoD4Radiant
    // R_AddCmdSetCustomShaderConstant 0x4fd330). The editor has no scene sun
    // (rgp.world->sunLight), so it injects the parsed worldspawn sun direction/colour
    // into the active source-state as code constants (SUN_POSITION/DIFFUSE/SPECULAR)
    // via this deferred command; RB_SetCustomConstantCmd writes them. kisak's CoD3
    // renderer has no equivalent (the game sets sun constants in-scene). Appended so
    // existing values hold; RC_COUNT bumps.
    RC_SET_CUSTOM_CONSTANT = 0x18,
    // KIWI-UX (ROUND BM): SECTION ANALYSIS — a D3D9 USER CLIP PLANE around the
    // editor's world passes (kiwi_section.cpp / camwnd.cpp's CamWnd_Draw bracket).
    // No CoD4Radiant or kisak counterpart: the editor is the only thing in either
    // tree that clips a view against an arbitrary world plane.  Appended so every
    // existing value holds; RC_COUNT bumps so R_GetCommandBuffer's
    // `renderCmd < RC_COUNT` guard admits it and RB_RenderCommandTable is sized for
    // it.  The whole feature is one enable/disable pair per camera frame.
    RC_SET_CLIP_PLANE = 0x19,
    // CoD4Radiant's deferred per-light constant payload (0x4fc330).
    RC_SET_LIGHT_COLOR = 0x1A,
    RC_COUNT = 0x1B,
#else
    RC_COUNT = 0x16,
#endif
};
enum GfxRenderTargetId : int
{                                       // ...
    R_RENDERTARGET_SAVED_SCREEN = 0x0,
    R_RENDERTARGET_FRAME_BUFFER = 0x1,
    R_RENDERTARGET_SCENE = 0x2,
    R_RENDERTARGET_RESOLVED_POST_SUN = 0x3,
    R_RENDERTARGET_RESOLVED_SCENE = 0x4,
    R_RENDERTARGET_FLOAT_Z = 0x5,
    R_RENDERTARGET_DYNAMICSHADOWS = 0x6,
    R_RENDERTARGET_PINGPONG_0 = 0x7,
    R_RENDERTARGET_PINGPONG_1 = 0x8,
    R_RENDERTARGET_SHADOWCOOKIE = 0x9,
    R_RENDERTARGET_SHADOWCOOKIE_BLUR = 0xA,
    R_RENDERTARGET_POST_EFFECT_0 = 0xB,
    R_RENDERTARGET_POST_EFFECT_1 = 0xC,
    R_RENDERTARGET_SHADOWMAP_SUN = 0xD,
    R_RENDERTARGET_SHADOWMAP_SPOT = 0xE,
    R_RENDERTARGET_COUNT = 0xF,
    R_RENDERTARGET_NONE = 0x10,
};


enum ShadowType : int
{                                       // ...
    SHADOW_NONE = 0x0,
    SHADOW_COOKIE = 0x1,
    SHADOW_MAP = 0x2,
};

enum GfxStencilOp : __int32
{
    GFXS_STENCILOP_KEEP = 0x0,
    GFXS_STENCILOP_ZERO = 0x1,
    GFXS_STENCILOP_REPLACE = 0x2,
    GFXS_STENCILOP_INCRSAT = 0x3,
    GFXS_STENCILOP_DECRSAT = 0x4,
    GFXS_STENCILOP_INVERT = 0x5,
    GFXS_STENCILOP_INCR = 0x6,
    GFXS_STENCILOP_DECR = 0x7,
    GFXS_STENCILOP_COUNT = 0x8,
};

enum GfxStencilFunc : __int32
{
    GFXS_STENCILFUNC_NEVER = 0x0,
    GFXS_STENCILFUNC_LESS = 0x1,
    GFXS_STENCILFUNC_EQUAL = 0x2,
    GFXS_STENCILFUNC_LESSEQUAL = 0x3,
    GFXS_STENCILFUNC_GREATER = 0x4,
    GFXS_STENCILFUNC_NOTEQUAL = 0x5,
    GFXS_STENCILFUNC_GREATEREQUAL = 0x6,
    GFXS_STENCILFUNC_ALWAYS = 0x7,
    GFXS_STENCILFUNC_COUNT = 0x8,
};

enum GfxProjectionTypes : __int32
{                                       // ...
    GFX_PROJECTION_2D = 0x0,
    GFX_PROJECTION_3D = 0x1,
};

enum MaterialTechniqueType : int
{                                       // ...
    TECHNIQUE_DEPTH_PREPASS = 0x0,
    TECHNIQUE_BUILD_FLOAT_Z = 0x1,
    TECHNIQUE_BUILD_SHADOWMAP_DEPTH = 0x2,
    TECHNIQUE_BUILD_SHADOWMAP_COLOR = 0x3,
    TECHNIQUE_UNLIT = 0x4,
    TECHNIQUE_EMISSIVE = 0x5,
    TECHNIQUE_EMISSIVE_SHADOW = 0x6,
    TECHNIQUE_LIT_BEGIN = 0x7,
    TECHNIQUE_LIT = 0x7,
    TECHNIQUE_LIT_SUN = 0x8,
    TECHNIQUE_LIT_SUN_SHADOW = 0x9,
    TECHNIQUE_LIT_SPOT = 0xA,
    TECHNIQUE_LIT_SPOT_SHADOW = 0xB,
    TECHNIQUE_LIT_OMNI = 0xC,
    TECHNIQUE_LIT_OMNI_SHADOW = 0xD,
    TECHNIQUE_LIT_INSTANCED = 0xE,
    TECHNIQUE_LIT_INSTANCED_SUN = 0xF,
    TECHNIQUE_LIT_INSTANCED_SUN_SHADOW = 0x10,
    TECHNIQUE_LIT_INSTANCED_SPOT = 0x11,
    TECHNIQUE_LIT_INSTANCED_SPOT_SHADOW = 0x12,
    TECHNIQUE_LIT_INSTANCED_OMNI = 0x13,
    TECHNIQUE_LIT_INSTANCED_OMNI_SHADOW = 0x14,
    TECHNIQUE_LIT_END = 0x15,
    TECHNIQUE_LIGHT_SPOT = 0x15,
    TECHNIQUE_LIGHT_OMNI = 0x16,
    TECHNIQUE_LIGHT_SPOT_SHADOW = 0x17,
    TECHNIQUE_FAKELIGHT_NORMAL = 0x18,
    TECHNIQUE_FAKELIGHT_VIEW = 0x19,
    TECHNIQUE_SUNLIGHT_PREVIEW = 0x1A,
    TECHNIQUE_CASE_TEXTURE = 0x1B,
    TECHNIQUE_WIREFRAME_SOLID = 0x1C,
    TECHNIQUE_WIREFRAME_SHADED = 0x1D,
    TECHNIQUE_SHADOWCOOKIE_CASTER = 0x1E,
    TECHNIQUE_SHADOWCOOKIE_RECEIVER = 0x1F,
    TECHNIQUE_DEBUG_BUMPMAP = 0x20,
    TECHNIQUE_DEBUG_BUMPMAP_INSTANCED = 0x21,
    TECHNIQUE_COUNT = 0x22,
    TECHNIQUE_TOTAL_COUNT = 0x23,
    TECHNIQUE_NONE = 0x24,
};
inline MaterialTechniqueType &operator++(MaterialTechniqueType &e) {
    e = static_cast<MaterialTechniqueType>(static_cast<int>(e) + 1);
    return e;
}
inline MaterialTechniqueType &operator++(MaterialTechniqueType &e, int i)
{
    ++e;
    return e;
}

enum FullscreenType : int
{                                       // ...
    FULLSCREEN_DISPLAY = 0x0,
    FULLSCREEN_MIXED = 0x1,
    FULLSCREEN_SCENE = 0x2,
};

struct GfxCmdHeader // sizeof=0x4
{                                       // ...
    uint16_t id;
    uint16_t byteCount;
};

struct GfxCmdStretchPic // sizeof=0x2C
{
    GfxCmdHeader header;
    const Material *material;
    float x;
    float y;
    float w;
    float h;
    float s0;
    float t0;
    float s1;
    float t1;
    GfxColor color;
};

struct GfxCmdClearScreen // sizeof=0x1C
{
    GfxCmdHeader header;
    uint8_t whichToClear;
    uint8_t stencil;
    // padding byte
    // padding byte
    float depth;
    float color[4];
};

struct GfxCmdProjectionSet // sizeof=0x8
{
    GfxCmdHeader header;
    GfxProjectionTypes projection;
};

struct GfxCmdSaveScreenSection // sizeof=0x18
{
    GfxCmdHeader header;
    float s0;
    float t0;
    float ds;
    float dt;
    int screenTimerId;
};

struct GfxCmdSaveScreen // sizeof=0x8
{
    GfxCmdHeader header;
    int screenTimerId;
};

struct GfxRenderTargetSurface // sizeof=0x8
{                                       // ...
    IDirect3DSurface9 *color;           // ...
    IDirect3DSurface9 *depthStencil;    // ...
};

struct GfxRenderTarget // sizeof=0x14
{                                       // ...
    GfxImage *image;                    // ...
    GfxRenderTargetSurface surface;     // ...
    uint width;                 // ...
    uint height;                // ...
};

struct StateBitsTable // sizeof=0x8
{                                       // ...
    int stateBits;                      // ...
    const char *name;                   // ...
};

struct GfxCmdArray // sizeof=0x10
{                                       // ...
    uint8_t *cmds;              // ...
    int usedTotal;
    int usedCritical;
    GfxCmdHeader *lastCmd;
};

struct GfxRenderCommandExecState // sizeof=0x4
{                                       // ...
    const void *cmd;                    // ...
};
struct GfxCmdDrawText2D // sizeof=0x54
{
    GfxCmdHeader header;
    float x;
    float y;
    float rotation;
    Font_s *font;
    float xScale;
    float yScale;
    GfxColor color;
    int maxChars;
    int renderFlags;
    int cursorPos;
    char cursorLetter;
    // padding byte
    // padding byte
    // padding byte
    GfxColor glowForceColor;
    int fxBirthTime;
    int fxLetterTime;
    int fxDecayStartTime;
    int fxDecayDuration;
    const Material *fxMaterial;
    const Material *fxMaterialGlow;
    float padding;
    char text[3];
    // padding byte
};

struct FxCodeMeshData // sizeof=0x10
{                                       // ...
    uint triCount;
    uint16_t *indices;
    uint16_t argOffset;
    uint16_t argCount;
    uint pad;
};

struct GfxParticleCloud // sizeof=0x40
{                                       // ...
    GfxScaledPlacement placement;
    float endpos[3];
    GfxColor color;
    float radius[2];
    uint pad[2];
};
union PackedLightingCoords // sizeof=0x4
{                                       // ...
    uint packed;
    uint8_t array[4];
};
struct GfxSModelCachedVertex // sizeof=0x20
{                                       // ...
    float xyz[3];
    GfxColor color;
    PackedTexCoords texCoord;
    PackedUnitVec normal;
    PackedUnitVec tangent;
    PackedLightingCoords baseLighting;
};
struct GfxModelLightingPatch // sizeof=0x28
{                                       // ...
    uint16_t modelLightingIndex;
    uint8_t primaryLightWeight;
    uint8_t colorsCount;
    uint8_t groundLighting[4];
    uint16_t colorsWeight[8];
    uint16_t colorsIndex[8];
};
struct GfxBackEndPrimitiveData // sizeof=0x4
{                                       // ...
    int hasSunDirChanged;
};
struct FxMarkMeshData // sizeof=0x10
{                                       // ...
    uint triCount;
    uint16_t *indices;
    uint16_t modelIndex;
    uint8_t modelTypeAndSurf;
    uint8_t pad0;
    uint pad1;
};

struct GfxDrawSurfListInfo // sizeof=0x28
{                                       // ...
    const GfxDrawSurf *drawSurfs;
    uint drawSurfCount;
    MaterialTechniqueType baseTechType; // ...
    const struct GfxViewInfo *viewInfo;
    float viewOrigin[4];
    const GfxLight *light;
    int cameraView;
};
struct PointLightPartition // sizeof=0x68
{                                       // ...
    GfxLight light;
    GfxDrawSurfListInfo info;
};
struct __declspec(align(16)) ShadowCookie // sizeof=0xC0
{                                       // ...
    GfxMatrix shadowLookupMatrix;
    float boxMin[3];
    float boxMax[3];
    GfxViewParms *shadowViewParms;
    float fade;
    uint sceneEntIndex;
    GfxDrawSurfListInfo casterInfo;
    GfxDrawSurfListInfo receiverInfo;
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
};
struct __declspec(align(16)) ShadowCookieList // sizeof=0x1210
{                                       // ...
    ShadowCookie cookies[24];
    uint cookieCount;
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
};
struct ShadowCookieCmd // sizeof=0x10
{                                       // ...
    const GfxViewParms *viewParmsDpvs;
    const GfxViewParms *viewParmsDraw;
    ShadowCookieList *shadowCookieList;
    int localClientNum;
};
struct GfxSunShadowProjection // sizeof=0x60
{                                       // ...
    float viewMatrix[4][4];
    float switchPartition[4];
    float shadowmapScale[4];
};
struct GfxSunShadowBoundingPoly // sizeof=0x78
{                                       // ...
    float snapDelta[2];
    int pointCount;
    float points[9][2];
    int pointIsNear[9];
};
struct __declspec(align(16)) GfxSunShadowPartition // sizeof=0x200
{                                       // ...
    GfxViewParms shadowViewParms;
    int partitionIndex;
    GfxViewport viewport;
    GfxDrawSurfListInfo info;
    GfxSunShadowBoundingPoly boundingPoly;
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
};
struct GfxSunShadow // sizeof=0x4A0
{                                       // ...
    GfxMatrix lookupMatrix;
    GfxSunShadowProjection sunProj;
    GfxSunShadowPartition partition[2]; // 0 = partitionNear, 1 = partitionFar
};
struct __declspec(align(16)) GfxSpotShadow // sizeof=0x1F0
{                                       // ...
    GfxViewParms shadowViewParms;
    GfxMatrix lookupMatrix;
    uint8_t shadowableLightIndex;
    uint8_t pad[3];
    const GfxLight* light;
    float fade;
    GfxDrawSurfListInfo info;
    GfxViewport viewport;
    GfxImage* image;
    GfxRenderTargetId renderTargetId;
    float pixelAdjust[4];
    int clearScreen;
    GfxMeshData* clearMesh;
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
};

struct GfxBackEndData;

struct __declspec(align(8)) GfxCmdBufInput // sizeof=0x430
{                                       // ...
    float consts[58][4];
    const GfxImage* codeImages[27];     // ...
    uint8_t codeImageSamplerStates[27]; // ...
    // padding byte
    const GfxBackEndData* data;         // ...
    // padding byte
    // padding byte
    // padding byte
    // padding byte
};

const struct GfxViewInfo // sizeof=0x67B0
{                                       // ...
    GfxViewParms viewParms;
    GfxSceneDef sceneDef;
    GfxViewport sceneViewport;
    GfxViewport displayViewport;
    GfxViewport scissorViewport;
    ShadowType dynamicShadowType;
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    ShadowCookieList shadowCookieList;
    int localClientNum;
    int isRenderingFullScreen;
    bool needsFloatZ;
    // padding byte
    // padding byte
    // padding byte
    GfxLight shadowableLights[255];
    uint shadowableLightCount;
    PointLightPartition pointLightPartitions[4];
    GfxMeshData pointLightMeshData[4];
    int pointLightCount;
    uint emissiveSpotLightIndex;
    GfxLight emissiveSpotLight;
    int emissiveSpotDrawSurfCount;
    GfxDrawSurf *emissiveSpotDrawSurfs;
    uint emissiveSpotLightCount;
    float blurRadius;
    float frustumPlanes[4][4];
    GfxDepthOfField dof;
    GfxFilm film;
    GfxGlow glow;
    const void *cmds;
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    GfxSunShadow sunShadow;
    uint spotShadowCount;
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    GfxSpotShadow spotShadows[4];
    GfxQuadMeshData *fullSceneViewMesh;
    GfxDrawSurfListInfo litInfo;
    GfxDrawSurfListInfo decalInfo;
    GfxDrawSurfListInfo emissiveInfo;
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    GfxCmdBufInput input;
};
const struct __declspec(align(16)) GfxBackEndData // sizeof=0x11E780
{                                       // ...
    uint8_t surfsBuffer[0x20000];
    FxCodeMeshData codeMeshes[2048];
    uint primDrawSurfsBuf[65536]; // ...
    GfxViewParms viewParms[28];
    uint8_t primaryLightTechType[13][256];
    float codeMeshArgs[256][4];
    GfxParticleCloud clouds[256];
    GfxDrawSurf drawSurfs[32768];
    GfxMeshData codeMesh;
    GfxSModelCachedVertex smcPatchVerts[8192];
    uint16_t smcPatchList[256];
    uint smcPatchCount;
    uint smcPatchVertsUsed;
    GfxModelLightingPatch modelLightingPatchList[4096];
    volatile long modelLightingPatchCount;
    GfxBackEndPrimitiveData prim;
    uint shadowableLightHasShadowMap[8];
    uint frameCount;
    int drawSurfCount;
    volatile long surfPos;
    volatile long gfxEntCount;
    GfxEntity gfxEnts[128];
    volatile long cloudCount;
    volatile long codeMeshCount;
    volatile long codeMeshArgsCount;
    volatile long markMeshCount;
    FxMarkMeshData markMeshes[1536];
    GfxMeshData markMesh;
    GfxVertexBufferState *skinnedCacheVb;
    IDirect3DQuery9 *endFence;
    uint8_t *tempSkinBuf;
    volatile long tempSkinPos;
    IDirect3DIndexBuffer9 *preTessIb;
    int viewParmCount;
    GfxFog fogSettings;
    GfxCmdArray *commands;              // ...
    uint viewInfoIndex;
    uint viewInfoCount;
    GfxViewInfo *viewInfo;
    const void *cmds;
    GfxLight sunLight;
    int hasApproxSunDirChanged;
    volatile uint primDrawSurfPos;
    uint *staticModelLit;
    DebugGlobals debugGlobals;
    uint drawType;
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
};

void __cdecl TRACK_r_rendercmds();
void __cdecl R_FreeGlobalVariable(void *var);
void __cdecl R_InitRenderCommands();
void __cdecl R_InitRenderBuffers();
void __cdecl R_InitDynamicMesh(
    GfxMeshData *mesh,
    uint indexCount,
    uint vertCount,
    uint vertSize);
void __cdecl R_InitRenderThread();
void __cdecl R_SyncRenderThread();
GfxCmdArray *R_ClearCmdList();
void __cdecl R_ReleaseThreadOwnership();
void __cdecl R_IssueRenderCommands(uint type);
void R_PerformanceCounters();
bool R_UpdateSkinCacheUsage();
char __cdecl R_HandOffToBackend(char type);
void __cdecl R_ToggleSmpFrameCmd(char type);
void __cdecl R_AbortRenderCommands();
void __cdecl R_BeginClientCmdList2D();
void __cdecl R_ClearClientCmdList2D();
void __cdecl R_BeginSharedCmdList();
void __cdecl R_AddCmdEndOfList();
GfxCmdHeader *__cdecl R_GetCommandBuffer(GfxRenderCommand renderCmd, int bytes);
#ifdef KISAK_RADIANT
// Editor line-batching bridge (cod3src\src\gfx_d3d\r_rendercmds.cpp). See the
// implementation at the end of r_rendercmds.cpp for the §11 signature notes.
struct GfxPointVertex;
void *__cdecl R_AddMultipleRendercommands(int bytes);
void __cdecl R_AddLineCmd(short count, char width, char dimension, GfxPointVertex *verts);
void __cdecl R_AddCmd_Line3D(short count, char width, GfxPointVertex *verts);
void __cdecl R_AddCmd_Line3DNoDepth(short count, char width, GfxPointVertex *verts);
void __cdecl R_AddCmd_Line2D(short count, char width, GfxPointVertex *verts);  // IDB 0x4fd180 (2D sibling)
// Editor point-batching bridge — IDB R_AddPointCmd @ 0x4fcf60 / R_AddPointCmd_W
// @ 0x4fd080 (the latter a thin dimension=3 wrapper). Emits RC_DRAW_POINTS (kisak
// already has RB_DrawPointsCmd). The clipper draws its 1-3 placed clip points as
// screen-space squares through R_AddPointCmd_W.
struct GfxCmdDrawPoints;
GfxCmdDrawPoints *__cdecl R_AddPointCmd(short pointCount, char size, char dimension, const GfxPointVertex *verts);
GfxCmdDrawPoints *__cdecl R_AddPointCmd_W(short pointCount, char size, const GfxPointVertex *verts);

// KIWI: bracket a pass whose line output should be emitted GROUPED BY COLOUR instead
// of interleaved.  See the definition in r_rendercmds.cpp.
void __cdecl R_Ed_BeginLineBucket();
void __cdecl R_Ed_EndLineBucket();
// Drain barrier: call before emitting anything that is not one of the bucket's lines.
void __cdecl R_Ed_FlushLineBucket();
// Per-frame magnitudes for the camera plots; reading them resets them.
void __cdecl R_Ed_LineBucketStats(int *lines, int *groups, int *cmds, int *spills);

// ── KIWI: THE BUCKET'S UNGROUPED CONTENTS, FOR THE PER-OBJECT SURF CACHE ──────
// The bucket holds segments UNGROUPED, in emit order, and groups them only at the
// flush — which is what lets a per-object cache store one object's lines on their own
// and still get ONE global grouping when the frame replays a mix of cached and live
// objects.  Read back a range with R_Ed_LineBucketRead, put it back later with
// R_Ed_LineBucketAddSegs; the colour is carried by the vertices themselves, so only
// width and dimension travel beside them.
// Mark = the current segment count.  Pair it with the FLUSH COUNT: a flush emits
// everything the bucket held and restarts the numbering, so a range noted before one
// no longer names the same segments and the caller must abandon its capture.
int  __cdecl R_Ed_LineBucketMark();
int  __cdecl R_Ed_LineBucketFlushCount();
// Copy segments [from, from+count) out.  `verts` receives 2*count vertices.
// false = out of range / the bucket is not open.
bool __cdecl R_Ed_LineBucketRead(int from, int count, GfxPointVertex *verts,
                                 unsigned char *widths, unsigned char *dimensions);
// Append previously read segments back into the open bucket, interning their colours
// through the same group table the live path uses.  false = it did not fit.
bool __cdecl R_Ed_LineBucketAddSegs(const GfxPointVertex *verts, const unsigned char *widths,
                                    const unsigned char *dimensions, int count);

// Editor begin-view command — IDB R_AddBeginViewCmd @ 0x4fc3a0 (r_rendercmds.cpp).
// Emits an RC_BEGIN_VIEW carrying the scene def + the GfxViewParms* the backend
// (RB_BeginViewCmd) hands to R_BeginView, establishing viewParms3D for the lines.
struct GfxCmdBeginView // 28 bytes: header(4) + GfxSceneDef(0x14) + viewParms(4)
{
    GfxCmdHeader header;
    GfxSceneDef sceneDef;
    const GfxViewParms *viewParms;
};
void __cdecl R_AddBeginViewCmd(const GfxSceneDef *sceneDef, const GfxViewParms *viewParms);
void __cdecl RB_BeginViewCmd(GfxRenderCommandExecState *execState);
// Editor surface-cache flush command (RC_DRAW_EDITOR_SKINNEDCACHED). Front-end emit
// is R_AddEditorSurfsCmd (r_ed_scene.cpp); the backend handler draws the accumulated
// brush/world meshes from the per-material editor vertex-buffer pool (r_ed_vertbuf.cpp).
void __cdecl RB_DrawEditorSkinnedCachedCmd(GfxRenderCommandExecState *execState);
// Editor material-colour command — feeds CONST_SRC_CODE_MATERIAL_COLOR to the $line
// shader (the editor's bare line draw skips normal per-material constant setup).
void __cdecl R_AddCmdSetMaterialColor(const float *color);
// Editor sun-preview custom shader-constant command — IDB R_AddCmdSetCustomShaderConstant
// @ 0x4fd330 (#26 layer C2). Injects the parsed worldspawn sun (CONST_SRC_CODE_SUN_*) into
// the source-state for the SUNLIGHT_PREVIEW pass; RB_SetCustomConstantCmd consumes it.
void __cdecl R_AddCmdSetCustomShaderConstant(unsigned int constant, float x, float y, float z, float w);
// ══════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND BM) — SECTION ANALYSIS: THE USER CLIP PLANE COMMAND
// ══════════════════════════════════════════════════════════════════════════════
// `plane` is a WORLD-SPACE plane in the standard (a,b,c,d) form, and the half-space
// that SURVIVES is  a*x + b*y + c*z + d >= 0.  The front end hands world space
// because it is the only space it knows; the backend handler does the one
// transform D3D9 requires (see below) using the ACTIVE view's own matrices, so the
// command cannot go stale against a view it was not emitted for.
//
// THE D3D9 CONTRACT, and it is the whole reason this is a render command and not a
// SetClipPlane call at the draw site.  Direct3D 9 interprets the planes given to
// IDirect3DDevice9::SetClipPlane in WORLD space only while the FIXED-FUNCTION
// pipeline is transforming vertices.  With a PROGRAMMABLE vertex shader — which is
// every draw CoD4 issues (R_HW_SetVertexShader, rb_shade.cpp:103) — the runtime has
// no world matrix to speak of and the planes are interpreted in CLIP SPACE, i.e.
// the space the vertex shader writes oPos in.  So the plane must be transformed by
// the inverse of the composed view-projection before it is handed over.
//
// THE TRANSFORM, derived in this tree's own convention rather than copied.  These
// matrices are ROW-VECTOR (camwnd.cpp's projection builds put the translate in
// m[3][*], and R_SetupViewProjectionMatrices composes viewProj = view * proj), so
//     clip = world * VP        and        world = clip * VP^-1.
// A plane P is tested as the dot product `world . P` with world = (x,y,z,1), so
//     world . P = (clip * VP^-1) . P = clip . (VP^-1 * P)
// and the clip-space plane is VP^-1 applied to P AS A COLUMN VECTOR:
//     Pclip[k] = sum_j inverseViewProjectionMatrix.m[k][j] * P[j].
// (That is the same operation the D3D docs describe as "transform the plane by the
// inverse-transpose of the view-projection" — a plane is a covector, so in the
// row-vector convention used here the transpose is already accounted for by
// multiplying VP^-1 on the LEFT of the column P instead of on the right of a row.)
//
// `enable` false disables clipping entirely and ignores `plane`.  The editor emits
// exactly one enable at the head of CamWnd_Draw's geometry and one disable at its
// tail, so the state can never leak into the ImGui overlay or a 2D view.
struct GfxCmdSetClipPlane // sizeof=0x18
{
    GfxCmdHeader header;
    int   enable;
    float plane[4];       // WORLD space; the backend transforms it to clip space
};

// Returns FALSE when the command buffer had no room for it.  That matters and is
// not decoration: RC_SET_CLIP_PLANE is above RC_FIRST_NONCRITICAL, so
// R_GetCommandBuffer is allowed to drop it on a full frame (which a big map reaches
// every frame — see the KISAK_RADIANT headroom notes in r_rendercmds.cpp).  A
// dropped ENABLE costs one unclipped frame and needs no repair; a dropped DISABLE
// would leave the plane enabled for the ImGui overlay and for every later frame, so
// the caller (kiwi_section.cpp) tracks the acknowledged state and re-issues the
// disable at the head of the next camera frame until one lands.
// ── KIWI-UX (ROUND BO, ITEM 1): …AND UNTIL THIS ROUND IT COULD NOT RETURN FALSE.
// A NULL check is not the test in the EDITOR build: R_GetCommandBuffer's
// KISAK_RADIANT overflow arm hands back an unlinked scratch slot rather than NULL,
// so every drop was reported as a success and the repair above never ran.  The
// definition tests `s_cmdList->lastCmd` instead, which the two failure paths clear.
// Any future editor command that needs to know whether it landed must copy that
// test — every other non-critical adder in this file still returns void and still
// cannot tell.
bool __cdecl R_AddCmdSetClipPlane(int enable, const float *plane);
void __cdecl RB_SetClipPlaneCmd(GfxRenderCommandExecState *execState);

// IDB layout: command header (4) + the complete 0x40-byte GfxLight.
struct GfxCmdSetLightColor // sizeof=0x44
{
    GfxCmdHeader header;
    GfxLight     light;
};
static_assert(sizeof(GfxCmdSetLightColor) == (sizeof(void *) == 8 ? 80 : 68), "GfxCmdSetLightColor");
void __cdecl RC_SetLightColor(const GfxLight *light);
void __cdecl RB_SetLightColorCmd(GfxRenderCommandExecState *execState);
// Editor full-screen colored quad — IDB R_AddCmdDrawFullScreenColoredQuad @ 0x4fc260
// (#26 sun-preview, R_SunPrev_Main). Emits RC_DRAW_FULL_SCREEN_COLORED_QUAD (the backend
// RB_DrawFullScreenColoredQuadCmd already exists in the CoD3 base). Used for the black-world
// multiply quad + the clear-stencil quad that frame the SUNLIGHT_PREVIEW lit draw.
void __cdecl R_AddCmdDrawFullScreenColoredQuad(
    float s0, float t0, float s1, float t1, const float *color, const Material *material);
// Editor world-space text command — IDB R_AddCmdDrawTextAtPosition @ 0x4fbe20.
// Emits an RC_DRAW_TEXT_3D (kisak already has RB_DrawText3DCmd); XY coordinate /
// view-name / entity-name labels go through this (P5.5 text path).
struct Font_s;
void __cdecl R_AddCmdDrawTextAtPosition(
    const char *text, Font_s *font, const float *origin,
    const float *xPixelStep, const float *yPixelStep, const float *color);
// Editor 2D image command — IDB R_AddCmdDraw2DImage @ 0x4fb5e0. A non-rejecting
// R_AddCmdDrawStretchPic: draws a world ("wc/") material's colormap into the 2D pass
// (the texture-window thumbnail grid). See the .cpp for the depth-buffer-rejection note.
void __cdecl R_AddCmdDraw2DImage(
    float x, float y, float w, float h,
    float s0, float t0, float s1, float t1,
    const float *color, Material *material);
#endif
DebugGlobals *R_ToggleSmpFrame();
GfxViewParms *__cdecl R_AllocViewParms();
void __cdecl R_AddCmdDrawStretchPic(
    float x,
    float y,
    float w,
    float h,
    float s0,
    float t0,
    float s1,
    float t1,
    const float *color,
    Material *material);
bool __cdecl Material_HasAnyFogableTechnique(const Material *material);
const MaterialTechnique *__cdecl Material_GetTechnique(const Material *material, MaterialTechniqueType techType);
MaterialTechniqueSet *__cdecl Material_GetTechniqueSet(const Material *material);
void __cdecl R_AddCmdDrawStretchPicFlipST(
    float x,
    float y,
    float w,
    float h,
    float s0,
    float t0,
    float s1,
    float t1,
    const float *color,
    Material *material);
void __cdecl R_AddCmdDrawStretchPicRotateXY(
    float x,
    float y,
    float w,
    float h,
    float s0,
    float t0,
    float s1,
    float t1,
    float angle,
    const float *color,
    Material *material);
void __cdecl R_AddCmdDrawStretchPicRotateST(
    float x,
    float y,
    float w,
    float h,
    float centerS,
    float centerT,
    float radiusST,
    float scaleFinalS,
    float scaleFinalT,
    float angle,
    const float *color,
    Material *material);
void __cdecl R_AddCmdDrawTextWithCursor(
    const char *text,
    int maxChars,
    Font_s *font,
    float x,
    float y,
    float xScale,
    float yScale,
    float rotation,
    const float *color,
    int style,
    int cursorPos,
    char cursor);
GfxCmdDrawText2D *__cdecl AddBaseDrawTextCmd(
    const char *text,
    int maxChars,
    Font_s *font,
    float x,
    float y,
    float xScale,
    float yScale,
    float rotation,
    const float *color,
    int style,
    int cursorPos,
    char cursor);
void __cdecl R_AddCmdDrawText(
    const char *text,
    int maxChars,
    Font_s *font,
    float x,
    float y,
    float xScale,
    float yScale,
    float rotation,
    const float *color,
    int style);
void __cdecl R_AddCmdDrawTextSubtitle(
    const char *text,
    int maxChars,
    Font_s *font,
    float x,
    float y,
    float xScale,
    float yScale,
    float rotation,
    const float *color,
    int style,
    const float *glowColor,
    bool cinematic);
char __cdecl SetDrawText2DGlowParms(GfxCmdDrawText2D *cmd, const float *color, const float *glowColor);
void __cdecl R_AddCmdDrawTextWithEffects(
    const char *text,
    int maxChars,
    Font_s *font,
    float x,
    float y,
    float xScale,
    float yScale,
    float rotation,
    const float *color,
    int style,
    const float *glowColor,
    Material *fxMaterial,
    Material *fxMaterialGlow,
    int fxBirthTime,
    int fxLetterTime,
    int fxDecayStartTime,
    int fxDecayDuration);
char __cdecl SetDrawText2DPulseFXParms(
    GfxCmdDrawText2D *cmd,
    Material *fxMaterial,
    Material *fxMaterialGlow,
    int fxBirthTime,
    int fxLetterTime,
    int fxDecayStartTime,
    int fxDecayDuration);
void __cdecl R_AddCmdDrawConsoleText(
    char *textPool,
    int poolSize,
    int firstChar,
    int charCount,
    Font_s *font,
    float x,
    float y,
    float xScale,
    float yScale,
    const float *color,
    int style);
GfxCmdDrawText2D *__cdecl AddBaseDrawConsoleTextCmd(
    char *textPool,
    int poolSize,
    int firstChar,
    int charCount,
    Font_s *font,
    float x,
    float y,
    float xScale,
    float yScale,
    const float *color,
    int style);
void __cdecl CopyPoolTextToCmd(char *textPool, int poolSize, int firstChar, int charCount, GfxCmdDrawText2D *cmd);
void __cdecl R_AddCmdDrawConsoleTextSubtitle(
    char *textPool,
    int poolSize,
    int firstChar,
    int charCount,
    Font_s *font,
    float x,
    float y,
    float xScale,
    float yScale,
    const float *color,
    int style,
    const float *glowColor);
void __cdecl R_AddCmdDrawConsoleTextPulseFX(
    char *textPool,
    int poolSize,
    int firstChar,
    int charCount,
    Font_s *font,
    float x,
    float y,
    float xScale,
    float yScale,
    const float *color,
    int style,
    const float *glowColor,
    int fxBirthTime,
    int fxLetterTime,
    int fxDecayStartTime,
    int fxDecayDuration,
    Material *fxMaterial,
    Material *fxMaterialGlow);
void __cdecl R_AddCmdDrawQuadPic(const float (*verts)[2], const float *color, Material *material);
void __cdecl R_BeginFrame();
void R_UpdateFrontEndDvarOptions();
void __cdecl R_SetInputCodeConstantFromVec4(GfxCmdBufInput *input, CodeConstant constant, const float *value);
void __cdecl R_SetInputCodeImageTexture(GfxCmdBufInput *input, MaterialTextureSource codeTexture, const GfxImage *image);
bool __cdecl R_LightTweaksModified();
void R_SetTestLods();
bool __cdecl R_AreAnyImageOverridesActive();
void R_SetOutdoorFeatherConst();
void __cdecl R_SetInputCodeConstant(GfxCmdBufInput *input, CodeConstant constant, float x, float y, float z, float w);
void R_EnvMapOverrideConstants();
void __cdecl R_EndFrame();
void __cdecl R_AddCmdClearScreen(int whichToClear, const float *color, float depth, uint8_t stencil);
void __cdecl R_AddCmdSaveScreen(uint screenTimerId);
void __cdecl R_AddCmdSaveScreenSection(
    float viewX,
    float viewY,
    float viewWidth,
    float viewHeight,
    uint screenTimerId);
void __cdecl R_AddCmdBlendSavedScreenShockBlurred(
    int fadeMsec,
    float viewX,
    float viewY,
    float viewWidth,
    float viewHeight,
    uint screenTimerId);
void __cdecl R_AddCmdBlendSavedScreenShockFlashed(
    float intensityWhiteout,
    float intensityScreengrab,
    float viewX,
    float viewY,
    float viewWidth,
    float viewHeight);
void __cdecl R_AddCmdDrawProfile();
void __cdecl R_AddCmdProjectionSet2D();
void __cdecl R_AddCmdProjectionSet3D(); // KISAK_SP
void __cdecl R_AddCmdProjectionSet(GfxProjectionTypes projection);
void __cdecl R_BeginRemoteScreenUpdate();
void __cdecl R_EndRemoteScreenUpdate();
void __cdecl R_PushRemoteScreenUpdate(int remoteScreenUpdateNesting);
int __cdecl R_PopRemoteScreenUpdate();
bool __cdecl R_IsInRemoteScreenUpdate();

void __cdecl R_InitTempSkinBuf();

void R_AddCmdSetViewportValues(int x, int y, int width, int height);

void __cdecl R_ShutdownDynamicMesh(GfxMeshData *mesh);
void __cdecl R_ShutdownRenderBuffers();

void __cdecl R_ShutdownRenderCommands();

void __cdecl R_BeginDebugFrame();
void __cdecl R_EndDebugFrame();

extern GfxBackEndData *frontEndDataOut;
extern GfxBackEndData s_backEndData[2];

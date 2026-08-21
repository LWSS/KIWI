#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_entthumb.cpp — KIWI-UX (ROUND AV, ITEM 3) implementation.  The design,
// the scope, the cache route and the reset story are all in kiwi_entthumb.h.
//
// NOTHING here is a new engine mechanism.  Every call is one an existing caller
// already makes, in the same order:
//
//   THE TARGET   RTT_BeginThumb / RTT_EndThumb            radiant_rtt.cpp
//                (the standalone 5th slot; see radiant_rtt.h)
//   THE FRAME    R_BeginFrame / R_BeginSharedCmdList / R_AddCmdClearScreen /
//                R_EndFrame / R_IssueRenderCommands / R_SortMaterials
//                — CamWnd_RenderToRT's sequence verbatim (camwnd.cpp:4844)
//   THE CAMERA   R_Ed_ProjectionWouldBeValid + R_Ed_SetSceneParms
//                — CamWnd_SetupScene's pair (camwnd.cpp:310/:316), with the
//                  ORTHOGRAPHIC GfxMatrix round M added there (camwnd.cpp:297)
//   THE MODEL    R_RegisterModel -> AddModelToModelInstBuff -> SkinModelInst ->
//                R_AddEditorSurfsCmd -> RemoveModelInstFromBuf
//                — Entity_UpdateModelInst's registration (entity.cpp:696-699)
//                  and DrawModels' skin (camwnd.cpp:1608-1613), minus the entity
//   THE GUARD    setjmp( g_radiantAssetLoadJmp ) + __try/__except
//                — Editor_InstanceAndSkinModel's bracket (camwnd.cpp:1629)
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include <csetjmp>                  // the model-load asset-drop recovery guard
#include "qe3.h"                    // eclass_t
#include "kiwi_entthumb.h"
#include "radiant_rtt.h"            // RTT_BeginThumb / RTT_EndThumb / RTT_ThumbSurface
#include "kiwi_texcache.h"          // KIWI-UX (CLEANUP, C-65): the by-name texture cache

#include <d3d9.h>
#include <gfx_d3d/r_gfx.h>          // GfxMatrix
#include <gfx_d3d/r_init.h>         // dx
#include <gfx_d3d/r_scene.h>        // R_Ed_SetSceneParms / R_Ed_ProjectionWouldBeValid
#include <gfx_d3d/r_rendercmds.h>   // R_BeginFrame/EndFrame, clear, MaterialTechniqueType
#include <gfx_d3d/r_model.h>        // R_RegisterModel
#include <xanim/xmodel.h>           // XModel, XModelBad, XModelGetBounds
#include <universal/com_math.h>     // AngleVectors
#include <universal/q_parse.h>      // Com_GetParseThreadInfo / negativeNumbers

#include <map>
#include <string>
#include <math.h>
#include <string.h>

// ── externs, each copied from its definition ────────────────────────────────
extern void  Radiant_FL_Log( const char *fmt, ... );                             // mainfrm.cpp
// r_ed_scene.cpp:318 — int __cdecl AddModelToModelInstBuff(XModel*, float*, float)
// (the same spelling entity.cpp:254 uses).  `axis` is TWELVE floats: origin[3] then
// axis[3][3] — r_ed_scene.cpp:342-345 reads axis[0..2] as the origin and
// AxisToQuat((vec3_t*)(axis+3), ...) for the rotation.  Returns a 1-BASED handle, 0 on
// failure (XModelBad).
extern int   AddModelToModelInstBuff( XModel *model, float *axis, float scale );  // r_ed_scene.cpp:358
extern void  RemoveModelInstFromBuf( int inst );                                  // r_ed_scene.cpp:410
// r_ed_scene.cpp:880 — the same declaration camwnd.cpp:1635 carries.
extern void  SkinModelInst( int instanceHandle, Material *checkhandle, int techType,
                            const int *colorPtr, int drawFlags );                 // r_ed_scene.cpp:880
extern void *R_AddEditorSurfsCmd();                                               // r_ed_scene.cpp:284
extern void  R_SortMaterials();                                                   // r_ed_scene.cpp:2009
// engine_stubs.cpp:222-223 — the editor asset-drop recovery frame.
extern int     g_radiantAssetLoadGuard;                                           // engine_stubs.cpp:222
extern jmp_buf g_radiantAssetLoadJmp;                                             // engine_stubs.cpp:223

namespace
{
    // 128 is the RT edge.  The tile is 84x64 (KENTB_TILE_W/H), so 128 gives the
    // browser a >1.5x supersample in the tight dimension and leaves headroom if the
    // tile ever grows.  Square, so the projection needs no aspect term.
    const int   KENTT_RT       = 128;
    // The isometric-ish view.  Pitch 20 deg down, yaw 200 deg: the camera sits off the
    // model's front-right and looks back and slightly down at it, which is the angle
    // round AU's ImDrawList tile already draws its bbox at (ProjectIso, 30 deg) read as
    // a real camera.  CoD characters face +X, so a yaw past 180 shows the face.
    const float KENTT_PITCH    = 20.0f;
    const float KENTT_YAW      = 200.0f;
    // Framing pad, and the ortho half-depth.  Depth is SYMMETRIC about the eye for the
    // reason camwnd.cpp:272-277 gives: in an ortho view the eye POINT is arbitrary along
    // the view axis, so a one-sided near plane clips what you are looking straight at.
    const float KENTT_PAD      = 1.12f;
    // The guard-band constant every projection in this build carries (camwnd.cpp:236).
    const float KENTT_GUARD    = 0.99951171875f;
    // Neutral studio background, deliberately not COLOR_CAMERABACK: a thumbnail grid
    // reads better against one flat tone than against the viewport's sky.
    const float KENTT_BG[4]    = { 0.13f, 0.14f, 0.17f, 1.0f };
    // ── KIWI-UX (CLEANUP, C-73): the "this pixel is still background" cutoff ────
    // The copy-out loop's lit/white classifier needs to know which 8-bit pixels are
    // the CLEAR colour rather than model.  KENTT_BG x 255 is (33.15, 35.7, 43.35),
    // i.e. (33, 36, 43) as written; the cutoff sits ~27 levels above each channel,
    // which is enough headroom for the A8R8G8B8 rounding and for the faint edge
    // blend a model silhouette leaves against the clear, and still far below any
    // lit surface.  Red and green share one number because they are within three
    // levels of each other.
    //
    // THESE ARE NOT COMPUTED FROM KENTT_BG AND CANNOT BE (it is a runtime float
    // array, not a constant expression).  Change KENTT_BG and you MUST re-derive
    // these three by hand, or every pixel is misclassified — silently, because the
    // only symptom is a white-tile warning that stops firing or starts lying.
    const int   KENTT_BG_MAX[3] = { 60, 60, 70 };

    // ── KIWI-UX (ROUND AX, ITEM 5) — THE LONG-AXIS YAW ──────────────────────
    // USER REPORT: "can you rotate all the weapons a bit so they show up better?"
    // They were not special-cased and they are not special-cased now; the rule is
    // geometric and applies to every model.  The camera is at yaw KENTT_YAW=200, so a
    // world direction at yaw t projects onto the screen horizontal with the factor
    // |sin(t - 20°)| (dot with the camera's right vector, which is
    // (sin 200°, -cos 200°) = (-0.342, 0.940)).  A weapon's long axis is model +X,
    // i.e. t = 0, giving |sin(-20°)| = 0.34: two thirds of its length is projected
    // AWAY from the viewer.  That is precisely "near edge-on".
    //   * full broadside would be t = 110° (factor 1.0) — a flat side elevation, the
    //     other bad extreme;
    //   * KENTT_LONG_AXIS_YAW = 75° is 35° off broadside, factor cos 35° = 0.82 — the
    //     3/4 presentation, which reads as a solid object rather than a silhouette.
    // Applied ONLY when one horizontal axis genuinely dominates, so characters, crates
    // and every roughly-square model keep the round-AV framing untouched.
    const float KENTT_LONG_AXIS_YAW = 75.0f;
    const float KENTT_LONG_AXIS_RATIO = 2.0f;   // "dominates" = longer than 2x the other

    // ── KIWI-UX (ROUND AX, ITEM 3) — THE BUDGET ─────────────────────────────
    // USER REPORT: "It lags while loading the model previews (can you put it on a
    // thread?)".  It cannot go on a thread — see kiwi_entthumb.h — so instead the
    // per-tick cost is SPACED OUT.  Round AV rendered one thumbnail EVERY tick, and
    // each one is a synchronous R_RegisterModel + every material + every image upload:
    // back-to-back multi-frame stalls, which is exactly what "lags" describes.  A
    // thumbnail now waits at least KENTT_MIN_GAP_MS, and additionally at least
    // KENTT_COST_FACTOR x however long the LAST one took — so a cheap model costs one
    // stall per ~4 ticks and an expensive one leaves proportionally more air behind it.
    const unsigned KENTT_MIN_GAP_MS   = 66;   // ~4 ticks of the 60 Hz pump
    const unsigned KENTT_COST_FACTOR  = 2;

    // ── KIWI-UX (ROUND AX, ITEM 4) — THE WHITE-TILE DETECTOR ────────────────
    // Round AW proved that CoD4's AC130 thermal characters ARE near-white and that
    // this is faithful.  What it could not do is tell a FAITHFUL white tile from a
    // BROKEN one without the operator pointing at it.  The copy-out loop already
    // touches every pixel, so classifying costs two counters: a tile whose lit pixels
    // are overwhelmingly near-white is named once, with its model, in the console.
    // Silent on a healthy grid, which is what makes it safe to leave in (D-AW5).
    const int   KENTT_WHITE_LEVEL = 235;    // per-channel, out of 255
    const float KENTT_WHITE_FRAC  = 0.85f;  // of the non-background pixels

    // KIWI-UX (CLEANUP, C-65): the entry struct, the map and the release loop are
    // kiwiTexEntry_t / kiwiTexCache_t / KiwiTexCache_ReleaseAll (kiwi_texcache.h)
    // — kiwi_skybox.cpp carried a character-identical copy of all three.  The
    // BUILD arms below stay here, because what makes a thumbnail and what makes a
    // cube-face tile are different questions.
    kiwiTexCache_t s_cache;
    IDirect3DSurface9                  *s_readback = nullptr;   // SYSTEMMEM, reused

    // At most ONE pending request, and it is data (not an eclass_t*): a .def reload
    // (Eclass_FreeAll) between the panel draw and the tick would dangle a class pointer,
    // and there is no reason to hold one — the model NAME is everything the render needs.
    bool s_haveReq = false;
    char s_reqClass[128];
    char s_reqModel[128];

    // ── the model name a CLASS carries, or null ─────────────────────────────
    // `defaultmdl=` -> default_model_name (eclass.cpp:958), which is cycleModelName[0]
    // (qe3.h:601-611).  A CLASS_PREFAB (classtype & 0x10) is excluded on purpose: its
    // "model" is a prefab of entities, not an XModel — Eclass_LoadModel takes the
    // Prefab_Load branch for it (eclass.cpp:1057) and there is no handle to skin.
    const char *ClassModelName( const eclass_t *ec )
    {
        if ( !ec )
            return nullptr;
        if ( ( ec->classtype & 0x10 /*CLASS_PREFAB*/ ) != 0 )
            return nullptr;
        const char *m = ec->default_model_name;
        return ( m && *m ) ? m : nullptr;
    }

    // ── the guarded model load ──────────────────────────────────────────────
    // MSVC forbids setjmp and __try in one function (C2713), so this is the same two-
    // function split camwnd.cpp:1599/:1565 uses.  It runs BEFORE any render target is
    // bound — see kiwi_entthumb.h on why a longjmp between Begin and End would be the
    // D3DERR_INVALIDCALL failure mode rather than a skipped model.
    XModel *RegisterSEH( const char *name )
    {
        XModel *m = nullptr;
        __try   { m = R_RegisterModel( name ); }
        __except ( EXCEPTION_EXECUTE_HANDLER ) { m = nullptr; }
        return m;
    }

    XModel *RegisterGuarded( const char *name )
    {
        // The same parse-state precondition Editor_InstanceAndSkinModel documents
        // (camwnd.cpp:1637-1644): XModel_LoadPhysicsCollMap parses at parseInfoNum 0 with
        // whatever tokenizer mode an earlier editor parse left, and collmap geometry holds
        // negative floats.  Identical result in either mode, so just enable it.
        ParseThreadInfo *parse = Com_GetParseThreadInfo();
        parseInfo_t     *pi    = &parse->parseInfo[parse->parseInfoNum];
        int              savedNeg = pi->negativeNumbers;
        pi->negativeNumbers = 1;

        if ( setjmp( g_radiantAssetLoadJmp ) != 0 )
        {
            g_radiantAssetLoadGuard = 0;    // unwound from an ERR_DROP
            pi->negativeNumbers = savedNeg;
            return nullptr;
        }
        ++g_radiantAssetLoadGuard;
        XModel *m = RegisterSEH( name );
        --g_radiantAssetLoadGuard;
        pi->negativeNumbers = savedNeg;
        return m;
    }

    // ── the skin, under SEH only ────────────────────────────────────────────
    // No setjmp here, and that is the point: this runs BETWEEN RTT_BeginThumb and
    // RTT_EndThumb, and __except unwinds back into THIS function rather than past the
    // caller's RTT_EndThumb.  The model is already registered by the time we get here, so
    // there is no asset load left to raise ERR_DROP.
    void SkinSEH( int inst, int techType )
    {
        __try
        {
            SkinModelInst( inst, nullptr, techType, nullptr, /*drawFlags*/ 0 );
            R_AddEditorSurfsCmd();
        }
        __except ( EXCEPTION_EXECUTE_HANDLER )
        {
        }
    }

    // ── RT -> SYSTEMMEM -> MANAGED ──────────────────────────────────────────
    // The one place the cache is made.  Returns null on any D3D failure, which the
    // caller turns into a FAILED entry (the tile keeps its bbox).
    // KIWI-UX (CLEANUP, C-67): `outD3DFail` separates "D3D would not give me the
    // pixels" from "this class has no usable model".  Every null below is the FIRST
    // kind, and the caller must NOT cache it as a permanent failure — a healthy
    // device that fails one readback used to mark the class FAILED forever and log a
    // line pointing the operator at the ASSET when the fault was D3D.
    IDirect3DTexture9 *CopyOutThumb( float *outWhiteFrac, bool *outD3DFail )
    {
        if ( outWhiteFrac )
            *outWhiteFrac = 0.0f;
        if ( outD3DFail )
            *outD3DFail = true;              // every early-out below is a D3D failure
        IDirect3DSurface9 *rt = RTT_ThumbSurface();
        if ( !rt || !dx.device )
            return nullptr;

        if ( !s_readback )
        {
            // SYSTEMMEM, so it is NOT a Reset casualty; released with the cache purely so
            // the whole feature has one teardown entry point.
            if ( dx.device->CreateOffscreenPlainSurface( KENTT_RT, KENTT_RT,
                                                         D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                         &s_readback, nullptr ) < 0 )
            {
                s_readback = nullptr;
                return nullptr;
            }
        }
        if ( dx.device->GetRenderTargetData( rt, s_readback ) < 0 )
            return nullptr;

        IDirect3DTexture9 *tex = nullptr;
        if ( dx.device->CreateTexture( KENTT_RT, KENTT_RT, 1, 0, D3DFMT_A8R8G8B8,
                                       D3DPOOL_MANAGED, &tex, nullptr ) < 0 || !tex )
            return nullptr;

        D3DLOCKED_RECT src = {};
        D3DLOCKED_RECT dst = {};
        if ( s_readback->LockRect( &src, nullptr, D3DLOCK_READONLY ) < 0 )
        {
            tex->Release();
            return nullptr;
        }
        if ( tex->LockRect( 0, &dst, nullptr, 0 ) < 0 )
        {
            s_readback->UnlockRect();
            tex->Release();
            return nullptr;
        }
        if ( outD3DFail )
            *outD3DFail = false;             // past every D3D gate
        // KIWI-UX (ROUND AX, ITEM 4): the white-tile classification rides this loop.
        // "Lit" = any pixel measurably brighter than KENTT_BG (0.13,0.14,0.17 -> ~33,36,43
        // in 8-bit), which after the clear is exactly "a model surface was drawn here".
        // The cutoff is KENTT_BG_MAX, derived beside KENTT_BG.
        int lit   = 0;
        int white = 0;
        for ( int y = 0; y < KENTT_RT; ++y )
        {
            const unsigned *s = (const unsigned *)( (const unsigned char *)src.pBits + (size_t)y * src.Pitch );
            unsigned       *d = (unsigned *)( (unsigned char *)dst.pBits + (size_t)y * dst.Pitch );
            for ( int x = 0; x < KENTT_RT; ++x )
            {
                const unsigned p = s[x] | 0xFF000000u;   // FORCE OPAQUE — see kiwi_entthumb.h
                d[x] = p;
                const int r = (int)( ( p >> 16 ) & 0xFF );
                const int g = (int)( ( p >>  8 ) & 0xFF );
                const int b = (int)(   p         & 0xFF );
                if ( r <= KENTT_BG_MAX[0] && g <= KENTT_BG_MAX[1] && b <= KENTT_BG_MAX[2] )
                    continue;                            // background
                ++lit;
                if ( r >= KENTT_WHITE_LEVEL && g >= KENTT_WHITE_LEVEL && b >= KENTT_WHITE_LEVEL )
                    ++white;
            }
        }
        tex->UnlockRect( 0 );
        s_readback->UnlockRect();
        if ( outWhiteFrac && lit > 0 )
            *outWhiteFrac = (float)white / (float)lit;
        return tex;
    }

    // ── the render ──────────────────────────────────────────────────────────
    // Returns the finished MANAGED texture, or null (caller marks FAILED).  Structured so
    // that every early-out is either BEFORE RTT_BeginThumb or AFTER RTT_EndThumb: there
    // is NO return at all between them.
    // KIWI-UX (CLEANUP, C-67): `outD3DFail` is forwarded from CopyOutThumb — true
    // means "D3D would not hand over the pixels", which is transient and must not be
    // cached.  A null return with it FALSE is the real "no usable model for this
    // class" answer.
    IDirect3DTexture9 *RenderThumb( const char *modelName, float *outWhiteFrac, bool *outD3DFail )
    {
        if ( outD3DFail )
            *outD3DFail = false;
        XModel *model = RegisterGuarded( modelName );
        if ( !model || XModelBad( model ) )
            return nullptr;                  // a MODEL failure, not a D3D one

        float mins[3], maxs[3];
        XModelGetBounds( model, mins, maxs );

        // ── KIWI-UX (ROUND AX, ITEM 5) — the long-axis yaw.  See KENTT_LONG_AXIS_YAW.
        // Decided from the model's OWN bounds, before framing, because the instance
        // rotation changes the bounds the framing has to fit.
        float modelYaw = 0.0f;
        {
            const float ex = maxs[0] - mins[0];
            const float ey = maxs[1] - mins[1];
            if ( ex > ey * KENTT_LONG_AXIS_RATIO )
                modelYaw = KENTT_LONG_AXIS_YAW;              // long axis is +X (yaw 0)
            else if ( ey > ex * KENTT_LONG_AXIS_RATIO )
                modelYaw = KENTT_LONG_AXIS_YAW - 90.0f;      // long axis is +Y (yaw 90)
        }
        const float yawRad = modelYaw * 0.017453292f;        // deg -> rad
        const float yawC   = (float)cos( yawRad );
        const float yawS   = (float)sin( yawRad );

        // The instance basis: rows are the model's local axes expressed in world, which
        // is the convention AddModelToModelInstBuff's AxisToQuat consumes
        // (r_ed_scene.cpp:342-345).  A pure yaw about Z, identity when modelYaw is 0.
        float place[12] = { 0.0f, 0.0f, 0.0f,
                            yawC, yawS, 0.0f,
                           -yawS, yawC, 0.0f,
                            0.0f, 0.0f, 1.0f };

        // Frame the ROTATED bounds, not the model's.  The instance sits at the world
        // origin, so a yaw moves the bbox in X/Y whenever the model's own centre is off
        // its origin (every character: z 0..72, x/y roughly centred but never exactly).
        // Transforming the 8 corners is the only way the ortho framing stays exact.
        float rmins[3] = {  1e30f,  1e30f,  1e30f };
        float rmaxs[3] = { -1e30f, -1e30f, -1e30f };
        for ( int c = 0; c < 8; ++c )
        {
            const float lx = ( c & 1 ) ? maxs[0] : mins[0];
            const float ly = ( c & 2 ) ? maxs[1] : mins[1];
            const float lz = ( c & 4 ) ? maxs[2] : mins[2];
            const float wp[3] = { lx * place[3] + ly * place[6] + lz * place[9],
                                  lx * place[4] + ly * place[7] + lz * place[10],
                                  lx * place[5] + ly * place[8] + lz * place[11] };
            for ( int i = 0; i < 3; ++i )
            {
                if ( wp[i] < rmins[i] ) rmins[i] = wp[i];
                if ( wp[i] > rmaxs[i] ) rmaxs[i] = wp[i];
            }
        }

        float center[3];
        float half = 0.0f;
        for ( int i = 0; i < 3; ++i )
        {
            center[i] = ( rmins[i] + rmaxs[i] ) * 0.5f;
            const float e = ( rmaxs[i] - rmins[i] ) * 0.5f;
            if ( e > half )
                half = e;
        }
        if ( !( half > 0.0f ) )
            return nullptr;                       // degenerate bounds — nothing to frame

        // The view basis, exactly CamWnd_BuildMatrix's convention (camwnd.cpp:192):
        // AngleVectors wants pitch NEGATED, and the scene axis is { vpn, -vright, vup }
        // (camwnd.cpp:220-223).
        float ang[3] = { -KENTT_PITCH, KENTT_YAW, 0.0f };
        float vpn[3], vright[3], vup[3];
        AngleVectors( ang, vpn, vright, vup );
        float axis[3][3] = {
            {  vpn[0],     vpn[1],     vpn[2]    },
            { -vright[0], -vright[1], -vright[2] },
            {  vup[0],     vup[1],     vup[2]    },
        };

        // ORTHOGRAPHIC, for two reasons.  It frames a bbox exactly (no fit iteration and
        // no perspective foreshortening across a grid of differently-sized models), and it
        // is a projection shape R_Ed_SetSceneParms already consumes every frame from the
        // 2D views — see camwnd.cpp:244-289 for the full argument and the matrix.
        const float halfExtent = half * KENTT_PAD;
        const float depth      = half * 8.0f + 64.0f;   // symmetric about the eye
        GfxMatrix proj;
        memset( &proj, 0, sizeof( proj ) );
        proj.m[0][0] = KENTT_GUARD / halfExtent;        // square RT: halfW == halfH
        proj.m[1][1] = KENTT_GUARD / halfExtent;
        proj.m[2][2] = KENTT_GUARD / ( 2.0f * depth );
        proj.m[3][2] = KENTT_GUARD * 0.5f;              // -zNear/(zFar-zNear) with zNear=-depth
        proj.m[3][3] = 1.0f;

        // The eye sits AT the bbox centre; with symmetric depth the model straddles 0 and
        // is fully inside [-depth, +depth] by construction.
        if ( !R_Ed_ProjectionWouldBeValid( center, (const float (*)[3])axis, &proj ) )
            return nullptr;

        // The instance: `place` was built above (origin at the world origin, the ROUND-AX
        // yaw as the 3x3).  Twelve floats, origin then the 3x3 (r_ed_scene.cpp:342-345).
        const int inst = AddModelToModelInstBuff( model, place, 1.0f );
        if ( !inst )
            return nullptr;

        IDirect3DTexture9 *out = nullptr;
        // KIWI-UX (CLEANUP, C-67): a thumb RT this device would not give us is a D3D
        // condition too, not a verdict on the asset.
        if ( !RTT_BeginThumb( KENTT_RT, KENTT_RT ) )
        {
            if ( outD3DFail )
                *outD3DFail = true;
        }
        else
        {
            R_BeginFrame();
            R_BeginSharedCmdList();
            R_AddCmdClearScreen( 7, KENTT_BG, 1.0f, 0 );   // colour+depth+stencil

            // ── KIWI-UX (ROUND AW, ITEM 1) — WHY THIS IS THE BINARY'S NEUTRAL AND NOT WHITE
            // USER REPORT: the browser tiles came out as untextured white silhouettes while
            // THE SAME MODELS drew textured in the 3D view.  This one command was the whole
            // difference, and it is a MATERIAL_COLOR difference, not an asset one:
            //
            //   * every character material's "unlit" technique is `vcsh`
            //     (raw/techsets/l_sm_r0c0n0s0.techset and the whole l_sm_* family), whose
            //     pixel shader is vertcol_shaded_fog.hlsl — its CTAB declares `materialColor`
            //     (raw/shader_bin/ps_2_0_68679706);
            //   * that is the vertcol_shaded family camwnd.cpp:2967-2972 documents:
            //         result.rgb = lerp( sample(colorMap)*vColor, matColor.rgb, matColor.w )
            //     so .w is a FLAT-COLOUR OVERRIDE FACTOR and w == 1 REPLACES THE TEXTURE;
            //   * round AV opened the thumbnail frame with {1,1,1,1} — copied from
            //     CamWnd_RenderToRT's frame opener (camwnd.cpp:4806-4807) — and then never
            //     reset it, so every thumbnail surf drew at w == 1: flat white.
            //
            // The CAMERA does not have this problem because it pushes the binary's neutral
            // {0,0,0,0} immediately before its own R_AddEditorSurfsCmd (camwnd.cpp:2973-2978,
            // the binary's R_SetMaterialColor(NULL) at 0x4080f7/0x408115).  The thumbnail now
            // opens with the same neutral: it draws ONLY the model flush, so there is nothing
            // in this frame that wants a non-neutral MATERIAL_COLOR at all.
            static const float s_flushNeutral[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            R_AddCmdSetMaterialColor( s_flushNeutral );
            R_Ed_SetSceneParms( center, (const float (*)[3])axis, &proj );

            // ── KIWI-UX (ROUND AW, ITEM 1) — OPEN THE PASS THE WAY EVERY CAMERA PASS OPENS
            // Cam_Draw calls R_SortMaterials before EACH accumulation (0x407ab9 world,
            // camwnd.cpp:2667; 0x407fbf tint, :2919; 0x4084f0 white) and round AV's thumbnail
            // only had the trailing one.  Two things depend on it here:
            //   * it advances sceneSurfCount_saved, so R_AddEditorSurfsCmd carries exactly
            //     this pass's surfs (r_ed_scene.cpp:1501);
            //   * it runs Material_Sort, which is the ONLY writer of
            //     info.drawSurf.fields.primarySortKey (r_material_load_obj.cpp:6792-6795) —
            //     and Editor_AddSurfCmd reads that field for the surf sort key
            //     (r_ed_scene.cpp:611).  RegisterGuarded above may have just loaded these
            //     materials for the first time (Material_Add sets rgp.needSortMaterials,
            //     r_material.cpp:537), and R_BeginFrame's own sort is gated on `rgp.world`
            //     (r_rendercmds.cpp:1304), which Radiant never has.
            R_SortMaterials();

            // The SKINNED draw item 1 restored, at the camera's textured technique.  A
            // thumbnail is always skinned regardless of View->Entities-as...: that menu is
            // about what the VIEWPORT shows, and a browser tile that went wireframe because
            // of a viewport preference would just be a worse bbox.
            SkinSEH( inst, (int)TECHNIQUE_UNLIT );

            R_EndFrame();
            R_IssueRenderCommands( (uint)-1 );
            // NOT optional and not just a sort: R_SortMaterials is the per-frame RESET of
            // the editor surf accumulation (r_ed_scene.cpp:273-285).  Without it the
            // model-surf cursor never rewinds.
            R_SortMaterials();
            RTT_EndThumb();

            out = CopyOutThumb( outWhiteFrac, outD3DFail );
        }

        RemoveModelInstFromBuf( inst );
        return out;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
IDirect3DTexture9 *KiwiEntThumb_Get( const eclass_t *ec, bool mayRequest )
{
    const char *cls = ec ? ec->name : nullptr;
    if ( !cls || !*cls )
        return nullptr;

    kiwiTexCache_t::iterator it = s_cache.find( cls );
    if ( it != s_cache.end() )
        return it->second.tex;          // null for a FAILED entry — the caller draws the bbox

    // ── KIWI-UX (ROUND AX, ITEM 3) — PRIORITISE WHAT THE OPERATOR CAN SEE.
    // Round AV's comment said "first VISIBLE requester wins", and the request order was
    // the tile submission order — but the browser has no clipper: kiwi_entbrowser.cpp
    // draws EVERY tile of every OPEN group (:722-733), so a scrolled-out class 400 rows
    // down was requesting before the row under the cursor.  The caller now passes the
    // tile's own ImGui::IsRectVisible result, so the queue is the visible run, top to
    // bottom of the scroll, which is the order the operator reads them in.
    if ( !mayRequest )
        return nullptr;

    // Not cached.  Only classes that name a model at the CLASS level get a request; the
    // rest are marked FAILED immediately so the lookup above answers them from then on and
    // this branch is never re-entered for them.
    const char *mdl = ClassModelName( ec );
    if ( !mdl )
    {
        s_cache[cls].failed = true;
        return nullptr;
    }

    // ONE request per frame.  First visible requester wins; the rest simply ask again next
    // frame, so a scrolled-to group warms in tile order at one per tick.  A collapsed
    // group submits no tiles at all (kiwi_entbrowser.cpp:680), so it costs nothing.
    if ( !s_haveReq )
    {
        strncpy( s_reqClass, cls, sizeof( s_reqClass ) - 1 );
        s_reqClass[sizeof( s_reqClass ) - 1] = '\0';
        strncpy( s_reqModel, mdl, sizeof( s_reqModel ) - 1 );
        s_reqModel[sizeof( s_reqModel ) - 1] = '\0';
        s_haveReq = true;
    }
    return nullptr;
}

void KiwiEntThumb_Tick()
{
    if ( !s_haveReq )
        return;                          // warm cache: zero cost, and this is the common path

    // ── KIWI-UX (ROUND AX, ITEM 3) — THE COST BUDGET.  See KENTT_MIN_GAP_MS.
    // The request is NOT consumed while the budget says wait: the tile asks again next
    // frame, and dropping it here would make a scroll that pauses on one row starve.
    static unsigned s_nextAllowed = 0;
    const unsigned  nowMs = ::GetTickCount();
    if ( s_nextAllowed && (int)( nowMs - s_nextAllowed ) < 0 )
        return;

    // Take the request FIRST.  Whatever happens below, this class is not asked again this
    // tick — and on success or hard failure it is never asked again at all.
    char cls[128], mdl[128];
    strncpy( cls, s_reqClass, sizeof( cls ) ); cls[sizeof( cls ) - 1] = '\0';
    strncpy( mdl, s_reqModel, sizeof( mdl ) ); mdl[sizeof( mdl ) - 1] = '\0';
    s_haveReq = false;

    float          whiteFrac = 0.0f;
    bool           d3dFail   = false;        // KIWI-UX (CLEANUP, C-67)
    const unsigned t0        = ::GetTickCount();
    IDirect3DTexture9 *tex   = RenderThumb( mdl, &whiteFrac, &d3dFail );
    const unsigned costMs    = ::GetTickCount() - t0;

    // Re-arm the budget from what this one actually cost.  A trivial prop leaves the
    // KENTT_MIN_GAP_MS floor; a 40 MB character with eight materials leaves twice its own
    // stall behind it, so the editor is interactive at least two thirds of the time while
    // a group warms.  This does NOT make the total wall time shorter — it cannot, the
    // loaders are synchronous — it makes the editor usable while it happens.
    {
        const unsigned gap = costMs * KENTT_COST_FACTOR;
        // KIWI-UX (CLEANUP, C-64): from the POST-render clock (t0 + costMs), not from
        // `nowMs`.  Basing the window on the PRE-render sample meant the render itself
        // consumed costMs of the 2*costMs window, leaving 1x its own stall as air
        // instead of 2x — ~50% interactive where the sentence above promises two
        // thirds.  This makes the code do what the prose already says.
        const unsigned after = t0 + costMs;
        s_nextAllowed = after + ( gap > KENTT_MIN_GAP_MS ? gap : KENTT_MIN_GAP_MS );
        if ( !s_nextAllowed )
            s_nextAllowed = 1u;
    }

    if ( tex )
    {
        kiwiTexEntry_t &e = s_cache[cls];
        // Safe as an IMMEDIATE Release, unlike the wizard previews: this runs from
        // KiwiEntThumb_Tick inside ImGuiShell_RenderViewportsToRT, BEFORE the pump authorizes
        // the frame, so no draw list is open and the previous one was presented a tick ago.
        if ( e.tex )
            e.tex->Release();            // cannot normally happen; cheap insurance
        e.tex    = tex;
        e.failed = false;

        // ── KIWI-UX (ROUND AX, ITEM 4) — NAME THE WHITE TILES, ONCE EACH ────────
        // USER REPORT: "some thumbnails still white".  Round AW proved the AC130 thermal
        // characters ARE near-white and that this is the shipped asset (§74, item 2); the
        // hole was that nothing in the editor could tell that case from a real one.  This
        // fires only for a tile that came out overwhelmingly white, prints model AND
        // class, and says which of the two it believes it is.  KiwiModelInfo
        // (KIWI_CMD_MODELINFO, round AW) is the follow-up that names the material and
        // techset — the authority is the techset, and a techset literally named `unlit`
        // is the thermal family.
        if ( whiteFrac >= KENTT_WHITE_FRAC )
        {
            const size_t n = strlen( mdl );
            const bool   thermal = ( n >= 6 && _stricmp( mdl + n - 6, "_ac130" ) == 0 );
            Radiant_FL_Log( "KiwiEntThumb: '%s' (model '%s') rendered %.0f%% white - %s",
                            cls, mdl, whiteFrac * 100.0f,
                            thermal
                              ? "an AC130 THERMAL model; near-white is the shipped asset (faithful)"
                              : "NOT a known thermal model - run KiwiModelInfo and re-open the "
                                "browser to dump its material/techset" );
        }
        return;
    }

    // No texture.  Distinguish "the device is not in a state to render" (transient —
    // leave the class UNCACHED so it is requested again once the device is back) from a
    // real failure (permanent — cache it so a missing asset costs one tick, ever).
    if ( !RTT_DeviceHealthy() )
        return;
    // KIWI-UX (CLEANUP, C-67): a HEALTHY device that still failed the readback is the
    // THIRD case, and it used to be swallowed by the second one — the class was marked
    // FAILED forever and the log pointed the operator at the asset when the fault was
    // D3D.  Left UNCACHED so the tile asks again, on the same retry-later semantics the
    // unhealthy-device arm above already has.
    if ( d3dFail )
    {
        Radiant_FL_Log( "KiwiEntThumb: D3D readback FAILED for '%s' (model '%s') - the "
                        "device is healthy, so this is not the asset; will retry",
                        cls, mdl );
        return;
    }
    s_cache[cls].failed = true;
    Radiant_FL_Log( "KiwiEntThumb: no preview for '%s' (model '%s') - keeping the bbox tile",
                    cls, mdl );
}

void KiwiEntThumb_ReleaseForReset()
{
    KiwiTexCache_ReleaseAll( s_cache );      // KIWI-UX (CLEANUP, C-65)
    if ( s_readback )
    {
        s_readback->Release();
        s_readback = nullptr;
    }
    s_haveReq = false;
}

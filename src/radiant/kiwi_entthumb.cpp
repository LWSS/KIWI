#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Model thumbnails reuse Radiant's RTT, camera, frame, model-skin, and asset-drop paths.
// Keep guarded registration outside RTT and preserve the camera-pass ordering below.

#include "stdafx.h"
#include <csetjmp>                  // the model-load asset-drop recovery guard
#include <imgui/imgui.h>            // GetFrameCount for the shared disk-load budget
#include "qe3.h"                    // eclass_t
#include "kiwi_entthumb.h"
#include "kiwi_thumbcache.h"        // executable-local .kthumb files
#include "radiant_rtt.h"            // RTT_BeginThumb / RTT_EndThumb / RTT_ThumbSurface
#include "kiwi_texcache.h"          // by-name texture cache

#include <d3d9.h>
#include <gfx_d3d/r_gfx.h>          // GfxMatrix
#include <gfx_d3d/r_init.h>         // dx
#include <gfx_d3d/r_scene.h>        // R_Ed_SetSceneParms / R_Ed_ProjectionWouldBeValid
#include <gfx_d3d/r_rendercmds.h>   // R_BeginFrame/EndFrame, clear, MaterialTechniqueType
#include <gfx_d3d/r_model.h>        // R_RegisterModel
#include <gfx_d3d/r_material.h>     // Material::info.name (tooltip stats)
#include <gfx_d3d/r_xsurface.h>     // XSurfaceGetNumTris / NumVerts (tooltip stats)
#include <xanim/xmodel.h>           // XModel, XModelBad, XModelGetBounds
#include <universal/com_math.h>     // AngleVectors
#include <universal/q_parse.h>      // Com_GetParseThreadInfo / negativeNumbers

#include <map>
#include <string>
#include <vector>
#include <float.h>
#include <math.h>
#include <string.h>

// Declarations mirror their definitions in the cited renderer files.
extern void  Radiant_FL_Log( const char *fmt, ... );                             // mainfrm.cpp
// r_ed_scene.cpp:335-375: `axis` is origin[3] then axis[3][3]; result is 1-based, 0 on failure.
extern int   AddModelToModelInstBuff( XModel *model, float *axis, float scale );  // r_ed_scene.cpp:375
extern void  RemoveModelInstFromBuf( int inst );                                  // r_ed_scene.cpp:427
extern void  SkinModelInst( int instanceHandle, Material *checkhandle, int techType,
                            const int *colorPtr, int drawFlags );                 // r_ed_scene.cpp:897
extern void *R_AddEditorSurfsCmd();                                               // r_ed_scene.cpp:301
extern void  R_SortMaterials();                                                   // r_ed_scene.cpp:2118
// Editor asset-drop recovery frame (engine_stubs.cpp:222-223).
extern int     g_radiantAssetLoadGuard;                                           // engine_stubs.cpp:222
extern jmp_buf g_radiantAssetLoadJmp;                                             // engine_stubs.cpp:223

namespace
{
    // 128 supersamples the 84x64 tile by >1.5x and keeps the projection aspect-free.
    const int   KENTT_RT       = 128;
    // Persisted in .kthumb headers; bump after any visual-output change.
    const unsigned KENTT_RENDER_VERSION = 1;
    // Source hashing plus cache-file upload is limited across both browser tabs.
    const int KENTT_DISK_LOADS_PER_FRAME = 8;
    // CoD characters face +X; pitch 20/yaw 200 shows the face from above and front-right.
    const float KENTT_PITCH    = 20.0f;
    const float KENTT_YAW      = 200.0f;
    // Add 12% framing pad; symmetric depth avoids a one-sided near plane (camwnd.cpp:272-277).
    const float KENTT_PAD      = 1.12f;
    // The guard-band constant every projection in this build carries (camwnd.cpp:236).
    const float KENTT_GUARD    = 0.99951171875f;
    // Use a fixed studio tone so the grid does not inherit the viewport sky.
    const float KENTT_BG[4]    = { 0.13f, 0.14f, 0.17f, 1.0f };
    // Background thresholds include BGRA8 rounding and silhouette-blend headroom.
    // Re-derive them by hand whenever KENTT_BG changes.
    const int   KENTT_BG_MAX[3] = { 60, 60, 70 };

    // A +X long axis is nearly edge-on at camera yaw 200; yaw 75 gives a three-quarter view.
    // Apply it only to strongly elongated bounds so characters and crates keep native yaw.
    const float KENTT_LONG_AXIS_YAW = 75.0f;
    const float KENTT_LONG_AXIS_RATIO = 2.0f;   // "dominates" = longer than 2x the other

    // Registration and dependent uploads are synchronous; spacing renders preserves UI time.
    const unsigned KENTT_MIN_GAP_MS   = 66;   // ~4 ticks of the 60 Hz pump
    const unsigned KENTT_COST_FACTOR  = 2;

    // Near-white AC130 thermal assets are legitimate; log other overwhelmingly white results.
    const int   KENTT_WHITE_LEVEL = 235;    // per-channel, out of 255
    const float KENTT_WHITE_FRAC  = 0.85f;  // of the non-background pixels

    // Generic by-name entry ownership and release live in kiwi_texcache.h.
    kiwiTexCache_t s_cache;
    IDirect3DSurface9 *s_readback = nullptr;   // SYSTEMMEM, reused

    struct modelMeta_t
    {
        bool  haveBounds = false;
        float mins[3] = { 0.0f, 0.0f, 0.0f };
        float maxs[3] = { 0.0f, 0.0f, 0.0f };
    };
    std::map<std::string, modelMeta_t> s_modelMeta;

    struct sourceState_t
    {
        bool                   haveHash   = false;
        bool                   diskChecked = false;
        kiwiThumbSourceHash_t  sourceHash = 0;
    };
    std::map<std::string, sourceState_t> s_sourceState;
    int      s_diskBudgetFrame = -1;
    int      s_diskLoadsThisFrame = 0;
    unsigned s_seenInvalidateSerial = 0;

    // Store request strings, not eclass_t*: Eclass_FreeAll may run before the next tick.
    bool        s_haveReq = false;
    std::string s_reqKey;
    std::string s_reqLabel;
    std::string s_reqModel;
    kiwiThumbSourceHash_t s_reqSourceHash = 0;

    bool TakeDiskLoadBudget()
    {
        const int frame = ImGui::GetFrameCount();
        if ( frame != s_diskBudgetFrame )
        {
            s_diskBudgetFrame = frame;
            s_diskLoadsThisFrame = 0;
        }
        if ( s_diskLoadsThisFrame >= KENTT_DISK_LOADS_PER_FRAME )
            return false;
        ++s_diskLoadsThisFrame;
        return true;
    }

    void ClearRequest()
    {
        s_reqKey.clear();
        s_reqLabel.clear();
        s_reqModel.clear();
        s_reqSourceHash = 0;
        s_haveReq = false;
    }

    void ClearThumbnailMemory()
    {
        KiwiTexCache_ReleaseAll( s_cache );
        s_modelMeta.clear();
        s_sourceState.clear();
        ClearRequest();
        s_diskBudgetFrame = -1;
        s_diskLoadsThisFrame = 0;
    }

    // `defaultmdl=` feeds default_model_name. CLASS_PREFAB is an entity prefab, not an
    // XModel handle, so exclude its Prefab_Load path (eclass.cpp:958, 1057).
    const char *ClassModelName( const eclass_t *ec )
    {
        if ( !ec )
            return nullptr;
        if ( ( ec->classtype & 0x10 /*CLASS_PREFAB*/ ) != 0 )
            return nullptr;
        const char *m = ec->default_model_name;
        return ( m && *m ) ? m : nullptr;
    }

    const char *BareModelName( const char *name )
    {
        if ( !name )
            return nullptr;
        if ( _strnicmp( name, "xmodel", 6 ) == 0 &&
             ( name[6] == '/' || name[6] == '\\' ) )
            return name + 7;
        return name;
    }

    // Retain the normalized name after FNV-1a so even a hash collision cannot alias models.
    std::string ModelCacheKey( const char *name )
    {
        const char *bare = BareModelName( name );
        unsigned __int64 hash = 14695981039346656037ui64;
        std::string normalized;
        for ( const unsigned char *p = (const unsigned char *)( bare ? bare : "" ); *p; ++p )
        {
            unsigned char c = *p;
            if ( c == '\\' )
                c = '/';
            if ( c >= 'A' && c <= 'Z' )
                c = (unsigned char)( c - 'A' + 'a' );
            normalized.push_back( (char)c );
            hash ^= c;
            hash *= 1099511628211ui64;
        }
        char prefix[32];
        _snprintf( prefix, sizeof( prefix ), "model:%016I64x:", hash );
        prefix[sizeof( prefix ) - 1] = '\0';
        return std::string( prefix ) + normalized;
    }

    bool BoundsValid( const float mins[3], const float maxs[3] )
    {
        for ( int i = 0; i < 3; ++i )
            if ( !_finite( mins[i] ) || !_finite( maxs[i] ) || maxs[i] < mins[i] )
                return false;
        return true;
    }

    // MSVC C2713 requires setjmp and __try in separate functions (camwnd.cpp:1565, 1599).
    // Registration runs before RTT binding so ERR_DROP cannot skip RTT_EndThumb.
    XModel *RegisterSEH( const char *name )
    {
        XModel *m = nullptr;
        __try   { m = R_RegisterModel( name ); }
        __except ( EXCEPTION_EXECUTE_HANDLER ) { m = nullptr; }
        return m;
    }

    XModel *RegisterGuarded( const char *name )
    {
        // Thumbnail ticks run at parseInfoNum 0; collmap parsing inherits that slot's
        // tokenizer state, so allow negative geometry (camwnd.cpp:1637-1644).
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

    // SEH returns inside the RTT bracket so RTT_EndThumb still runs; asset loading is done.
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

    // Build the managed/disk cache through SYSTEMMEM. `outD3DFail` keeps transient
    // readback failures out of the permanent model-failure cache.
    IDirect3DTexture9 *CopyOutThumb( const char *modelName,
                                     kiwiThumbSourceHash_t sourceHash,
                                     float *outWhiteFrac, bool *outD3DFail )
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
            // SYSTEMMEM survives Reset but shares the feature's single teardown path.
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
        // Count non-background and near-white pixels while copying; thresholds accompany KENTT_BG.
        int lit   = 0;
        int white = 0;
        std::vector<unsigned> pixels( KENTT_RT * KENTT_RT );
        for ( int y = 0; y < KENTT_RT; ++y )
        {
            const unsigned *s = (const unsigned *)( (const unsigned char *)src.pBits + (size_t)y * src.Pitch );
            unsigned       *d = (unsigned *)( (unsigned char *)dst.pBits + (size_t)y * dst.Pitch );
            for ( int x = 0; x < KENTT_RT; ++x )
            {
                const unsigned p = s[x] | 0xFF000000u;   // FORCE OPAQUE — see kiwi_entthumb.h
                d[x] = p;
                pixels[(size_t)y * KENTT_RT + x] = p;
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
        // Disk persistence is best-effort and cannot invalidate the in-memory result.
        KiwiThumbCache_Write( modelName, sourceHash, KENTT_RENDER_VERSION,
                              KENTT_RT, KENTT_RT, KIWI_THUMBCACHE_FORMAT_BGRA8,
                              &pixels[0], KENTT_RT * sizeof( unsigned ) );
        return tex;
    }

    // All exits occur outside the RTT begin/end bracket. `outD3DFail` distinguishes
    // transient render/readback failures from permanent model failures.
    IDirect3DTexture9 *RenderThumb( const char *modelName,
                                     kiwiThumbSourceHash_t sourceHash,
                                     float *outWhiteFrac,
                                     bool *outD3DFail, float outMins[3],
                                    float outMaxs[3], bool *outHaveBounds )
    {
        if ( outD3DFail )
            *outD3DFail = false;
        if ( outHaveBounds )
            *outHaveBounds = false;
        XModel *model = RegisterGuarded( modelName );
        if ( !model || XModelBad( model ) )
            return nullptr;                  // a MODEL failure, not a D3D one

        float mins[3], maxs[3];
        XModelGetBounds( model, mins, maxs );
        if ( BoundsValid( mins, maxs ) )
        {
            if ( outMins && outMaxs )
                for ( int i = 0; i < 3; ++i )
                {
                    outMins[i] = mins[i];
                    outMaxs[i] = maxs[i];
                }
            if ( outHaveBounds )
                *outHaveBounds = true;
        }

        // Choose long-axis yaw before framing because rotation changes the fitted bounds.
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

        // Rows are local axes in world space, matching AxisToQuat (r_ed_scene.cpp:359-362).
        float place[12] = { 0.0f, 0.0f, 0.0f,
                            yawC, yawS, 0.0f,
                           -yawS, yawC, 0.0f,
                            0.0f, 0.0f, 1.0f };

        // Transform all eight corners because yaw can shift an off-origin model's world bounds.
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

        // CamWnd_BuildMatrix negates pitch and passes {vpn, -vright, vup}
        // (camwnd.cpp:192, 220-223).
        float ang[3] = { -KENTT_PITCH, KENTT_YAW, 0.0f };
        float vpn[3], vright[3], vup[3];
        AngleVectors( ang, vpn, vright, vup );
        float axis[3][3] = {
            {  vpn[0],     vpn[1],     vpn[2]    },
            { -vright[0], -vright[1], -vright[2] },
            {  vup[0],     vup[1],     vup[2]    },
        };

        // Orthographic projection avoids perspective size variation and matches the 2D-view path
        // (camwnd.cpp:244-289).
        const float halfExtent = half * KENTT_PAD;
        const float depth      = half * 8.0f + 64.0f;   // symmetric about the eye
        GfxMatrix proj;
        memset( &proj, 0, sizeof( proj ) );
        proj.m[0][0] = KENTT_GUARD / halfExtent;        // square RT: halfW == halfH
        proj.m[1][1] = KENTT_GUARD / halfExtent;
        proj.m[2][2] = KENTT_GUARD / ( 2.0f * depth );
        proj.m[3][2] = KENTT_GUARD * 0.5f;              // -zNear/(zFar-zNear) with zNear=-depth
        proj.m[3][3] = 1.0f;

        // Center the eye and use symmetric depth so the model straddles view-space zero.
        if ( !R_Ed_ProjectionWouldBeValid( center, (const float (*)[3])axis, &proj ) )
            return nullptr;

        // `place` is world origin followed by the yaw basis (r_ed_scene.cpp:359-362).
        const int inst = AddModelToModelInstBuff( model, place, 1.0f );
        if ( !inst )
            return nullptr;

        IDirect3DTexture9 *out = nullptr;
        // Failure to acquire the thumbnail RT is transient D3D state, not an asset verdict.
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

            // vcsh treats materialColor.a == 1 as a flat-color override that replaces texture.
            // Match the camera's neutral R_SetMaterialColor(NULL) at 0x4080f7/0x408115.
            static const float s_flushNeutral[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            R_AddCmdSetMaterialColor( s_flushNeutral );
            R_Ed_SetSceneParms( center, (const float (*)[3])axis, &proj );

            // Match Cam_Draw's pre-accumulation sort (0x407ab9/0x407fbf/0x4084f0): it
            // advances sceneSurfCount_saved and assigns primarySortKey before submission,
            // including for materials registered above.
            R_SortMaterials();

            // Always use the textured technique; viewport wireframe preferences do not apply here.
            SkinSEH( inst, (int)TECHNIQUE_UNLIT );

            R_EndFrame();
            R_IssueRenderCommands( (uint)-1 );
            // This trailing sort resets editor-surface accumulation (r_ed_scene.cpp:290-302).
            R_SortMaterials();
            RTT_EndThumb();

            out = CopyOutThumb( modelName, sourceHash, outWhiteFrac, outD3DFail );
        }

        RemoveModelInstFromBuf( inst );
        return out;
    }
}

namespace
{
    IDirect3DTexture9 *GetByModelName( const char *modelName, const char *label,
                                      bool mayRequest )
    {
        const char *bare = BareModelName( modelName );
        if ( !bare || !*bare )
            return nullptr;

        const std::string key = ModelCacheKey( bare );
        kiwiTexCache_t::iterator it = s_cache.find( key );
        if ( it != s_cache.end() )
            return it->second.tex;
        if ( !mayRequest )
            return nullptr;

        sourceState_t &source = s_sourceState[key];
        if ( !source.diskChecked )
        {
            if ( !TakeDiskLoadBudget() )
                return nullptr;
            if ( !source.haveHash )
            {
                float mins[3] = { 0.0f, 0.0f, 0.0f };
                float maxs[3] = { 0.0f, 0.0f, 0.0f };
                bool haveBounds = false;
                if ( !KiwiThumbCache_SourceHash( bare, &source.sourceHash,
                                                 mins, maxs, &haveBounds ) )
                    return nullptr;
                source.haveHash = true;
                if ( haveBounds )
                {
                    modelMeta_t &meta = s_modelMeta[key];
                    meta.haveBounds = true;
                    for ( int i = 0; i < 3; ++i )
                    {
                        meta.mins[i] = mins[i];
                        meta.maxs[i] = maxs[i];
                    }
                }
            }

            IDirect3DTexture9 *diskTexture = nullptr;
            const kiwiThumbCacheLoadResult_t load = KiwiThumbCache_Load(
                bare, source.sourceHash, KENTT_RENDER_VERSION,
                KENTT_RT, KENTT_RT, KIWI_THUMBCACHE_FORMAT_BGRA8,
                &diskTexture );
            if ( load == KIWI_THUMBCACHE_RETRY )
                return nullptr;
            source.diskChecked = true;
            if ( load == KIWI_THUMBCACHE_HIT && diskTexture )
            {
                kiwiTexEntry_t &entry = s_cache[key];
                entry.tex = diskTexture;
                entry.failed = false;
                return diskTexture;
            }
        }

        // Both tabs share one pending render; disk hits may consume all eight load slots.
        if ( !s_haveReq )
        {
            s_reqKey   = key;
            s_reqLabel = ( label && *label ) ? label : bare;
            s_reqModel = bare;
            s_reqSourceHash = source.sourceHash;
            s_haveReq  = true;
        }
        return nullptr;
    }
}

IDirect3DTexture9 *KiwiEntThumb_Get( const eclass_t *ec, bool mayRequest )
{
    const char *cls = ec ? ec->name : nullptr;
    const char *mdl = ClassModelName( ec );
    if ( !cls || !*cls || !mdl )
        return nullptr;
    return GetByModelName( mdl, cls, mayRequest );
}

IDirect3DTexture9 *KiwiEntThumb_GetModel( const char *xmodelName, bool mayRequest )
{
    return GetByModelName( xmodelName, xmodelName, mayRequest );
}

bool KiwiEntThumb_GetModelBounds( const char *xmodelName,
                                  float outMins[3], float outMaxs[3] )
{
    if ( !xmodelName || !*xmodelName || !outMins || !outMaxs )
        return false;
    const std::string key = ModelCacheKey( xmodelName );
    std::map<std::string, modelMeta_t>::const_iterator it = s_modelMeta.find( key );
    if ( it == s_modelMeta.end() || !it->second.haveBounds )
        return false;
    for ( int i = 0; i < 3; ++i )
    {
        outMins[i] = it->second.mins[i];
        outMaxs[i] = it->second.maxs[i];
    }
    return true;
}

bool KiwiEntThumb_ModelFailed( const char *xmodelName )
{
    if ( !xmodelName || !*xmodelName )
        return false;
    const std::string key = ModelCacheKey( xmodelName );
    kiwiTexCache_t::const_iterator it = s_cache.find( key );
    return it != s_cache.end() && it->second.failed;
}

bool KiwiEntThumb_ModelStats( const char *xmodelName, kiwiModelStats_t *out )
{
    if ( !xmodelName || !*xmodelName || !out )
        return false;
    memset( out, 0, sizeof( *out ) );
    if ( KiwiEntThumb_ModelFailed( xmodelName ) )
        return false;
    XModel *model = RegisterGuarded( xmodelName );
    if ( !model || XModelBad( model ) )
        return false;

    out->lods      = XModelGetNumLods( model );
    out->bones     = XModelNumBones( model );
    out->collSurfs = model->numCollSurfs;
    for ( int lod = 0; lod < out->lods && lod < 4; ++lod )
    {
        const int surfCount = (int)XModelGetSurfCount( model, lod );
        for ( int s = 0; s < surfCount; ++s )
        {
            const XSurface *surf = XModelGetSurface( model, lod, s );
            if ( !surf )
                continue;
            const int tris  = XSurfaceGetNumTris( surf );
            const int verts = XSurfaceGetNumVerts( surf );
            out->totalTris += tris;
            if ( lod == 0 )
            {
                ++out->lod0Surfs;
                out->lod0Tris  += tris;
                out->lod0Verts += verts;
            }
        }
        if ( lod != 0 )
            continue;
        Material **skins = XModelGetSkins( model, 0 );
        for ( int s = 0; skins && s < surfCount; ++s )
        {
            const Material *m = skins[s];
            const char *name = ( m && m->info.name ) ? m->info.name : nullptr;
            if ( !name || !name[0] )
                continue;
            bool dup = false;
            for ( int k = 0; k < out->materialCount && !dup; ++k )
                dup = ( _stricmp( out->materials[k], name ) == 0 );
            if ( dup || out->materialCount >= 8 )
                continue;
            strncpy( out->materials[out->materialCount], name, sizeof( out->materials[0] ) - 1 );
            out->materials[out->materialCount][sizeof( out->materials[0] ) - 1] = '\0';
            ++out->materialCount;
        }
    }
    return true;
}

void KiwiEntThumb_Tick()
{
    const unsigned invalidateSerial = KiwiThumbCache_InvalidateSerial();
    if ( !s_seenInvalidateSerial )
        s_seenInvalidateSerial = invalidateSerial;
    else if ( invalidateSerial != s_seenInvalidateSerial )
    {
        // Release invalidated textures before ImGui can reference them in a draw list.
        ClearThumbnailMemory();
        s_seenInvalidateSerial = invalidateSerial;
    }

    if ( !s_haveReq )
        return;                          // warm cache: zero cost, and this is the common path

    // Do not consume while throttled; a stationary visible row must retain its request.
    static unsigned s_nextAllowed = 0;
    const unsigned  nowMs = ::GetTickCount();
    if ( s_nextAllowed && (int)( nowMs - s_nextAllowed ) < 0 )
        return;

    // Consume before rendering so this request cannot run twice in one tick.
    const std::string key   = s_reqKey;
    const std::string label = s_reqLabel;
    const std::string mdl   = s_reqModel;
    const kiwiThumbSourceHash_t sourceHash = s_reqSourceHash;
    ClearRequest();

    float          whiteFrac = 0.0f;
    bool           d3dFail   = false;        // transient renderer/readback failure
    float          mins[3]   = { 0.0f, 0.0f, 0.0f };
    float          maxs[3]   = { 0.0f, 0.0f, 0.0f };
    bool           haveBounds = false;
    const unsigned t0        = ::GetTickCount();
    IDirect3DTexture9 *tex   = RenderThumb( mdl.c_str(), sourceHash,
                                             &whiteFrac, &d3dFail,
                                             mins, maxs, &haveBounds );
    const unsigned costMs    = ::GetTickCount() - t0;

    if ( haveBounds )
    {
        modelMeta_t &meta = s_modelMeta[key];
        meta.haveBounds = true;
        for ( int i = 0; i < 3; ++i )
        {
            meta.mins[i] = mins[i];
            meta.maxs[i] = maxs[i];
        }
    }

    // Leave at least the fixed gap, or KENTT_COST_FACTOR times this synchronous stall.
    {
        const unsigned gap = costMs * KENTT_COST_FACTOR;
        // Start from the post-render clock so rendering does not consume the idle gap.
        const unsigned after = t0 + costMs;
        s_nextAllowed = after + ( gap > KENTT_MIN_GAP_MS ? gap : KENTT_MIN_GAP_MS );
        if ( !s_nextAllowed )
            s_nextAllowed = 1u;
    }

    if ( tex )
    {
        kiwiTexEntry_t &e = s_cache[key];
        // Tick runs before ImGui frame authorization, so no draw list can retain e.tex.
        if ( e.tex )
            e.tex->Release();            // cannot normally happen; cheap insurance
        e.tex    = tex;
        e.failed = false;

        // AC130 thermal assets are legitimately near-white; log other white results with
        // model/class context for follow-up through KiwiModelInfo.
        if ( whiteFrac >= KENTT_WHITE_FRAC )
        {
            const size_t n = mdl.size();
            const bool   thermal = ( n >= 6 && _stricmp( mdl.c_str() + n - 6, "_ac130" ) == 0 );
            Radiant_FL_Log( "KiwiEntThumb: '%s' (model '%s') rendered %.0f%% white - %s",
                            label.c_str(), mdl.c_str(), whiteFrac * 100.0f,
                            thermal
                              ? "an AC130 THERMAL model; near-white is the shipped asset (faithful)"
                              : "NOT a known thermal model - run KiwiModelInfo and re-open the "
                                "browser to dump its material/techset" );
        }
        return;
    }

    // Device-state failures stay uncached; permanent model failures use the bbox fallback.
    if ( !RTT_DeviceHealthy() )
        return;
    // A healthy-device D3D failure is also transient and must remain requestable.
    if ( d3dFail )
    {
        Radiant_FL_Log( "KiwiEntThumb: D3D readback FAILED for '%s' (model '%s') - the "
                        "device is healthy, so this is not the asset; will retry",
                        label.c_str(), mdl.c_str() );
        return;
    }
    s_cache[key].failed = true;
    Radiant_FL_Log( "KiwiEntThumb: no preview for '%s' (model '%s') - keeping the bbox tile",
                    label.c_str(), mdl.c_str() );
}

void KiwiEntThumb_ReleaseForReset()
{
    ClearThumbnailMemory();
    if ( s_readback )
    {
        s_readback->Release();
        s_readback = nullptr;
    }
}

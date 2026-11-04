#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// KIWI (2026-11-02): camera preview of effects - see kiwi_fxpreview.h.
//
// The maths follows the game's FX code in src/EffectsCore (same repo):
//   spawn schedule      fx_update.cpp FX_BeginLooping / FX_SpawnLoopingElems / FX_SpawnOneShotElems,
//                       fx_system.cpp FX_SpawnElem (delay + life ranges, inclusive integer randoms)
//   spawn position      fx_update_util.cpp FX_GetSpawnOrigin / FX_OffsetSpawnOrigin,
//                       fx_system.cpp FX_GetOriginForElem, FX_GetOrientation (run-relative frames)
//   velocity            fx_convert.cpp FX_SampleVelocityInFrame (local/world frames, useRand*,
//                       back-compat amplitude), fx_update.cpp FX_IntegrateVelocityInSegment,
//                       FX_NextElementPosition (gravity = value * 0.01 * 800 units/s^2)
//   visuals             fx_convert.cpp FX_SampleVisualState (one random for colour+alpha,
//                       modColorByAlpha, size = 0.5 * scale * curve), fx_draw.cpp FX_EvaluateSize
//                       (size1 only with nonUniformScale), FX_EvaluateDistanceFade,
//                       FX_GetSpriteTexCoords (atlas), FX_GenSpriteVerts / FX_DrawElem_Tail.
// Randoms come from a per-particle generator instead of the game's 479-entry table, so
// individual particles differ from the game; the distributions are the same.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"            // camera_s

#include <gfx_d3d/r_gfx.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_rendercmds.h>
#include <gfx_d3d/r_state.h>    // GFXS0_* / GFXS1_* (the twin materials' state bits)
#include <gfx_d3d/r_init.h>     // dx (thumbnail readback)
#include <gfx_d3d/r_scene.h>    // R_Ed_SetSceneParms / R_Ed_ProjectionWouldBeValid
#include <d3d9.h>
#include "radiant_rtt.h"        // RTT_BeginThumb / RTT_EndThumb / RTT_ThumbSurface

#include "kiwi_fxpreview.h"
#include "kiwi_efx.h"
#include "kiwi_lines.h"         // KiwiTris_FillNeutral
#include "kiwi_particles.h"
#include "kiwi_ported.h"
#include "radiant_registry.h"

#include <float.h>
#include <math.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

extern int __cdecl R_Ed_CmdBufferHeadroom();                                // r_rendercmds.cpp

namespace
{
    const char *KFXP_PROFILE      = "KiwiFxPreview";
    const int   KFXP_SAMPLES      = 32;          // velocity / rotation integral table intervals
    const int   KFXP_MAX_PARTS    = 6000;        // live particles across every instance
    const int   KFXP_MAX_CHILDREN = 48;          // runner children per instance
    const int   KFXP_MAX_DEPTH    = 4;           // runner nesting
    const float KFXP_ALL_RADIUS   = 3000.0f;     // "All" mode: FX entities within this of the camera
    const int   KFXP_REPEAT_PAUSE = 1000;        // one-shot effects replay after this many ms
    const int   KFXP_PREWARM_MAX  = 4000;        // looping effects start already running (ms)
    const int   KFXP_QUAD_BYTES   = 4 * ( 16 + 12 + 4 + 8 ) + 12 * 2 + 32;
    const float KFXP_PI           = 3.14159265358979f;

    // ── compiled definitions ─────────────────────────────────────────────────
    struct elemRt_t
    {
        kefxElem_t src;
        std::vector<Material *> mats;            // per visual (sprites/tails/clouds)
        std::vector<Material *> draw;            // what is drawn for each: EditorTwin( mats[i] )
        std::vector<std::string> missing;
        int   colBits = 0, rowBits = 0, entryCount = 1;
        // Integrated displacement per life-second at t = i / KFXP_SAMPLES (base + r * amp).
        float locBase[KFXP_SAMPLES + 1][3], locAmp[KFXP_SAMPLES + 1][3];
        float wldBase[KFXP_SAMPLES + 1][3], wldAmp[KFXP_SAMPLES + 1][3];
        bool  hasLoc = false, hasWld = false;
        float rotBase[KFXP_SAMPLES + 1], rotAmp[KFXP_SAMPLES + 1];   // radians per life-second
        bool  drawable = false;
    };

    struct defRt_t
    {
        std::string name;
        unsigned long long stamp = 0;
        DWORD checkedTick = 0;
        bool  ok = false;
        bool  looping = false;
        int   maxLifeMsec = 0;
        std::string err;
        std::vector<elemRt_t> elems;
    };

    std::map<std::string, std::unique_ptr<defRt_t> > s_defs;     // by lower-case fx name
    std::map<std::string, kefxDoc_t>                 s_override; // editor working copies

    std::string Key( const char *fx )
    {
        std::string k = fx ? fx : "";
        for ( size_t i = 0; i < k.size(); ++i )
        {
            k[i] = (char)tolower( (unsigned char)k[i] );
            if ( k[i] == '\\' ) k[i] = '/';
        }
        return k;
    }

    // FX_SampleVelocityInFrame on a KFXP_SAMPLES table; `world` selects the absVel frames.
    void BuildVelocity( elemRt_t &rt, bool world )
    {
        const kefxElem_t &e = rt.src;
        const unsigned ef = e.editorFlags;
        const bool useVel[2] = { ( ( ef & KEFX_E_ABSVEL0 ) != 0 ) == world, ( ( ef & KEFX_E_ABSVEL1 ) != 0 ) == world };
        const bool useRand[2] = { useVel[0] && ( ef & KEFX_E_RANDVEL0 ), useVel[1] && ( ef & KEFX_E_RANDVEL1 ) };
        const bool broken = ( ef & KEFX_E_BACKCOMPATVEL ) != 0;
        float ( *base )[3] = world ? rt.wldBase : rt.locBase;
        float ( *amp )[3]  = world ? rt.wldAmp : rt.locAmp;
        float prevB[3] = { 0, 0, 0 }, prevA[3] = { 0, 0, 0 };
        bool any = false;
        for ( int i = 0; i <= KFXP_SAMPLES; ++i )
        {
            const float t = (float)i / (float)KFXP_SAMPLES;
            float vb[3] = { 0, 0, 0 }, va[3] = { 0, 0, 0 };
            for ( int a = 0; a < 3; ++a )
            {
                if ( useVel[0] )
                {
                    const float s = KiwiEfx_Sample( e.vel[0][a].curve[0], 1, t ) * e.vel[0][a].scale;
                    vb[a] += s;
                    if ( useRand[0] ) va[a] -= s;
                }
                if ( useVel[1] )
                {
                    const float s = KiwiEfx_Sample( e.vel[1][a].curve[0], 1, t ) * e.vel[1][a].scale;
                    vb[a] += s;
                    if ( useRand[broken ? 0 : 1] ) va[a] -= s;
                }
                if ( useRand[0] ) va[a] += KiwiEfx_Sample( e.vel[0][a].curve[1], 1, t ) * e.vel[0][a].scale;
                if ( useRand[1] ) va[a] += KiwiEfx_Sample( e.vel[1][a].curve[1], 1, t ) * e.vel[1][a].scale;
            }
            for ( int a = 0; a < 3; ++a )
            {
                if ( i == 0 )
                {
                    base[0][a] = 0.0f;
                    amp[0][a]  = 0.0f;
                }
                else
                {
                    base[i][a] = base[i - 1][a] + ( prevB[a] + vb[a] ) * 0.5f / (float)KFXP_SAMPLES;
                    amp[i][a]  = amp[i - 1][a] + ( prevA[a] + va[a] ) * 0.5f / (float)KFXP_SAMPLES;
                    if ( fabsf( base[i][a] ) > 1e-4f || fabsf( amp[i][a] ) > 1e-4f )
                        any = true;
                }
                prevB[a] = vb[a];
                prevA[a] = va[a];
            }
        }
        ( world ? rt.hasWld : rt.hasLoc ) = any;
    }

    // FX_SampleVisualState rotation: deg/s graph integrated over life (radians per life-second).
    void BuildRotation( elemRt_t &rt )
    {
        const kefxElem_t &e = rt.src;
        const bool rnd = ( e.editorFlags & KEFX_E_RANDROTDELTA ) != 0;
        float prevB = 0.0f, prevA = 0.0f;
        for ( int i = 0; i <= KFXP_SAMPLES; ++i )
        {
            const float t = (float)i / (float)KFXP_SAMPLES;
            const float b = KiwiEfx_Sample( e.rot.curve[0], 1, t ) * e.rot.scale * KFXP_PI / 180.0f;
            const float a = rnd ? KiwiEfx_Sample( e.rot.curve[1], 1, t ) * e.rot.scale * KFXP_PI / 180.0f - b : 0.0f;
            if ( i == 0 )
                rt.rotBase[0] = rt.rotAmp[0] = 0.0f;
            else
            {
                rt.rotBase[i] = rt.rotBase[i - 1] + ( prevB + b ) * 0.5f / (float)KFXP_SAMPLES;
                rt.rotAmp[i]  = rt.rotAmp[i - 1] + ( prevA + a ) * 0.5f / (float)KFXP_SAMPLES;
            }
            prevB = b;
            prevA = a;
        }
    }

    bool DrawsSprites( int type )
    {
        return type == KEFX_BILLBOARD || type == KEFX_ORIENTED || type == KEFX_TAIL || type == KEFX_CLOUD;
    }

    // The effect shaders (vertcol_simple_dtex, effect_add_dtex ...) draw NOTHING through the
    // editor's immediate triangle path: every quad reaches the backend with the right
    // geometry, colour, texture and state bits (checked with the fx_preview_info verb and
    // white / world-material substitutes), yet the frame stays empty.  The camera already
    // draws missing-material faces with $default, whose "2d" techset's unlit technique is
    // vertcol_simple (colour map x vertex colour, no _dtex decompress).  So each effect
    // material is drawn through a runtime twin: a duplicate of $default carrying the
    // effect's own colour map, blend mode and alpha test, with no depth write and no culling
    // (sprites never occlude, oriented sprites show both sides).  (The l_sm "vcsh" technique
    // the reference images use drew the texture but ignored the vertex RGB - colour graphs
    // had no effect.)  Distortion materials (heat haze) need the scene behind them; skipped.
    const char *KFXP_TWIN_BASE = "$default";

    Material *EditorTwin( Material *fx )
    {
        static std::map<Material *, Material *> s_twins;     // handles live as long as the editor
        std::map<Material *, Material *>::iterator found = s_twins.find( fx );
        if ( found != s_twins.end() )
            return found->second;
        Material *&twin = s_twins[fx];
        twin = nullptr;
        const Material *src = Material_FromHandle( fx );
        if ( !src || !src->stateBitsTable || !src->textureTable )
            return nullptr;
        const char *techSet = src->techniqueSet && src->techniqueSet->name ? src->techniqueSet->name : "";
        if ( strstr( techSet, "distortion" ) )
            return nullptr;
        GfxImage *image = nullptr;
        uint8_t samplerState = 0;
        for ( int t = 0; t < (int)src->textureCount && !image; ++t )
            if ( src->textureTable[t].semantic == TS_COLOR_MAP )
            {
                image = src->textureTable[t].u.image;
                samplerState = src->textureTable[t].samplerState;   // filtering / mips / clamp
            }
        const uint8_t se = src->stateBitsEntry[TECHNIQUE_UNLIT];
        static Material *s_base = nullptr;
        static bool s_baseTried = false;
        if ( !s_baseTried )
        {
            s_baseTried = true;
            s_base = Material_RegisterHandle( KFXP_TWIN_BASE, IMAGE_TRACK_MISC );   // the default material itself
            if ( !s_base )
                Sys_Printf( "FX preview: twin base material '%s' is missing; effects cannot draw.\n", KFXP_TWIN_BASE );
        }
        if ( !image || se == 0xFF || !s_base )
            return nullptr;
        const Material *base = Material_FromHandle( s_base );
        char name[192];
        _snprintf( name, sizeof( name ), "kiwi_fxtwin_%p_%s", (void *)fx, src->info.name ? src->info.name : "" );
        name[sizeof( name ) - 1] = '\0';
        Material *dup = Material_Duplicate( s_base, name );
        if ( !dup || dup == s_base || !dup->textureTable || dup->textureTable == base->textureTable
          || !dup->stateBitsTable || dup->stateBitsTable == base->stateBitsTable )
            return nullptr;
        for ( int t = 0; t < (int)dup->textureCount; ++t )
            if ( dup->textureTable[t].semantic == TS_COLOR_MAP )
            {
                dup->textureTable[t].u.image = image;
                dup->textureTable[t].samplerState = samplerState;   // not $default's point sampling
            }
        const unsigned s0 = src->stateBitsTable[se].loadBits[0];
        const unsigned keep0 = GFXS0_BLEND_RGB_MASK | GFXS0_BLEND_ALPHA_MASK | GFXS0_ATEST_DISABLE | GFXS0_ATEST_MASK;
        for ( int e = 0; e < (int)dup->stateBitsCount; ++e )
        {
            unsigned int &b0 = dup->stateBitsTable[e].loadBits[0];
            unsigned int &b1 = dup->stateBitsTable[e].loadBits[1];
            if ( b0 & GFXS0_POLYMODE_LINE )
                continue;
            b0 = ( b0 & ~( keep0 | (unsigned)GFXS0_CULL_MASK ) ) | ( s0 & keep0 ) | GFXS0_CULL_NONE;
            b1 = ( b1 & ~(unsigned)( GFXS1_DEPTHWRITE | GFXS1_DEPTHTEST_DISABLE | GFXS1_DEPTHTEST_MASK ) )
               | GFXS1_DEPTHTEST_LESSEQUAL;
        }
        twin = dup;
        return twin;
    }

    void Compile( defRt_t &d, const kefxDoc_t &doc )
    {
        d.elems.clear();
        d.looping = false;
        d.maxLifeMsec = 0;
        for ( size_t i = 0; i < doc.elems.size(); ++i )
        {
            const kefxElem_t &src = doc.elems[i];
            if ( src.Disabled() )
                continue;
            d.elems.push_back( elemRt_t() );
            elemRt_t &rt = d.elems.back();
            rt.src = src;
            BuildVelocity( rt, false );
            BuildVelocity( rt, true );
            BuildRotation( rt );
            rt.drawable = DrawsSprites( src.type ) && !src.visuals.empty();
            if ( rt.drawable )
            {
                for ( size_t v = 0; v < src.visuals.size(); ++v )
                {
                    const char *name = src.visuals[v].c_str();
                    if ( !strcmp( name, "$default" ) ) name = "$default3d";       // FX_RegisterMaterial
                    Material *m = Material_RegisterHandle( name, IMAGE_TRACK_FX );
                    if ( m && Material_IsDefault( m ) && strcmp( name, "$default3d" ) )
                        m = nullptr;
                    if ( !m )
                        rt.missing.push_back( src.visuals[v] );
                    rt.mats.push_back( m );
                    rt.draw.push_back( m ? EditorTwin( m ) : nullptr );
                }
                // FX_ConvertAtlas: the bit counts come from the first material's atlas size.
                if ( rt.mats[0] )
                {
                    const Material *info = Material_FromHandle( rt.mats[0] );
                    const int rows = info ? info->info.textureAtlasRowCount : 1;
                    const int cols = info ? info->info.textureAtlasColumnCount : 1;
                    while ( ( 1 << rt.rowBits ) < rows && rt.rowBits < 8 ) ++rt.rowBits;
                    while ( ( 1 << rt.colBits ) < cols && rt.colBits < 8 ) ++rt.colBits;
                }
                rt.entryCount = 1 << ( rt.colBits + rt.rowBits );
            }
            // Endless loops only: a looping element with a count (a burst of 80 sparks) ends,
            // so its effect replays like a one-shot instead of starting pre-warmed.
            if ( src.Looping() && src.loopCount <= 0 )
                d.looping = true;
            const int life = (int)( src.lifeSpanMsec.base + src.lifeSpanMsec.amp + src.spawnDelayMsec.base
                                    + src.spawnDelayMsec.amp );
            d.maxLifeMsec = ( std::max )( d.maxLifeMsec, life );
        }
        d.ok = true;
    }

    defRt_t *Def( const std::string &name )
    {
        if ( name.empty() )
            return nullptr;
        const std::string key = Key( name.c_str() );
        std::unique_ptr<defRt_t> &slot = s_defs[key];
        std::map<std::string, kefxDoc_t>::const_iterator ov = s_override.find( key );
        if ( ov != s_override.end() )
        {
            if ( !slot )
            {
                slot.reset( new defRt_t() );
                slot->name = name;
                Compile( *slot, ov->second );
            }
            return slot.get();                   // SetOverride recompiles it on every edit
        }
        // The file is re-checked twice a second: an outside edit (or a save) reloads it.
        const DWORD tick = ::GetTickCount();
        if ( slot && tick - slot->checkedTick < 500 )
            return slot.get();
        const unsigned long long stamp = KiwiEfx_FileStamp( name.c_str() );
        if ( slot )
        {
            slot->checkedTick = tick;
            if ( slot->stamp == stamp && ( stamp != 0 || !slot->err.empty() ) )
                return slot.get();               // unchanged, or still missing
        }
        else
            slot.reset( new defRt_t() );
        defRt_t &d = *slot;
        d.name = name;
        d.checkedTick = tick;
        d.stamp = stamp;
        kefxDoc_t doc;
        std::string err;
        if ( !KiwiEfx_Load( name.c_str(), doc, err ) )
        {
            if ( d.err != err )
                Sys_Printf( "FX preview: %s\n", err.c_str() );
            d.err = err;
            d.ok = false;
            d.elems.clear();
            return &d;
        }
        d.err.clear();
        Compile( d, doc );
        return &d;
    }

    // ── per-particle random numbers ──────────────────────────────────────────
    struct rng_t
    {
        unsigned s;
        explicit rng_t( unsigned seed ) : s( seed ? seed : 0x9E3779B9u ) {}
        float Next()
        {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            return (float)( s & 0xFFFFFF ) / (float)0x1000000;
        }
    };
    unsigned s_seedCounter = 0x1234567u;

    enum { R_VX, R_VY, R_VZ, R_ORGX, R_ORGY, R_ORGZ, R_YAW, R_HEIGHT, R_RADIUS, R_GRAV, R_SIZE0, R_SIZE1,
           R_SCALE, R_COLOR, R_ROT, R_ATLAS, R_ANG0, R_ANG1, R_ANG2, R_AVEL0, R_AVEL1, R_AVEL2, R_COUNT };

    struct frame_t
    {
        float org[3];
        float axis[3][3];       // forward, left, up (the game's spatial frame)
    };

    void AnglesToFrame( const float ang[3], const float org[3], frame_t &f )
    {
        const float p = ang[0] * KFXP_PI / 180.0f, y = ang[1] * KFXP_PI / 180.0f, r = ang[2] * KFXP_PI / 180.0f;
        const float sp = sinf( p ), cp = cosf( p ), sy = sinf( y ), cy = cosf( y ), sr = sinf( r ), cr = cosf( r );
        f.axis[0][0] = cp * cy;                 f.axis[0][1] = cp * sy;                 f.axis[0][2] = -sp;
        // left = -right
        f.axis[1][0] = -( -sr * sp * cy + cr * sy ); f.axis[1][1] = -( -sr * sp * sy - cr * cy ); f.axis[1][2] = sr * cp;
        f.axis[2][0] = cr * sp * cy + sr * sy;  f.axis[2][1] = cr * sp * sy - sr * cy;  f.axis[2][2] = cr * cp;
        memcpy( f.org, org, sizeof( f.org ) );
    }

    void LocalToWorld( const frame_t &f, const float local[3], float out[3] )
    {
        for ( int k = 0; k < 3; ++k )
            out[k] = f.org[k] + local[0] * f.axis[0][k] + local[1] * f.axis[1][k] + local[2] * f.axis[2][k];
    }

    void WorldToLocal( const frame_t &f, const float world[3], float out[3] )
    {
        float d[3] = { world[0] - f.org[0], world[1] - f.org[1], world[2] - f.org[2] };
        for ( int a = 0; a < 3; ++a )
            out[a] = d[0] * f.axis[a][0] + d[1] * f.axis[a][1] + d[2] * f.axis[a][2];
    }

    void Normalize( float v[3] )
    {
        const float l = sqrtf( v[0] * v[0] + v[1] * v[1] + v[2] * v[2] );
        if ( l > 1e-8f ) { v[0] /= l; v[1] /= l; v[2] /= l; }
    }

    void Cross( const float a[3], const float b[3], float o[3] )
    {
        o[0] = a[1] * b[2] - a[2] * b[1];
        o[1] = a[2] * b[0] - a[0] * b[2];
        o[2] = a[0] * b[1] - a[1] * b[0];
    }

    float Lerp( const kefxRange_t &r, float u ) { return r.base + r.amp * u; }
    int   IntRand( const kefxRange_t &r, float u )                     // ((amp+1) * r16) >> 16
    {
        const int base = (int)floorf( r.base + 0.5f ), amp = (int)floorf( r.amp + 0.5f );
        return base + ( amp > 0 ? (int)( u * (float)( amp + 1 ) ) : 0 );
    }

    // ── instances ────────────────────────────────────────────────────────────
    struct particle_t
    {
        int   elem;
        int   msecBegin;
        int   life;
        int   sequence;
        float rnd[R_COUNT];
        float origin[3];        // world (run world) or local to the run frame
        frame_t orient;         // fixed run frame (spawn / offset); unused for world & effect runs
    };

    struct instance_t
    {
        std::string fx;
        frame_t     frame;              // now
        frame_t     frameAtStart;       // when the effect (re)started (runRelToSpawn)
        double      msecNow = 0.0;
        int         msecSpawned = -1;   // spawns up to this time are done
        std::vector<int> loopSpawned;   // per elem: looping elems spawned
        bool        oneShotDone = false;
        int         idleSince = -1;     // a finished one-shot waits KFXP_REPEAT_PAUSE, then replays
        int         depth = 0;
        bool        repeat = true;      // child effects play once
        bool        seen = true;
        std::vector<particle_t> parts;
        std::vector<std::unique_ptr<instance_t> > children;
        unsigned    seed = 1;
    };

    std::map<const void *, std::unique_ptr<instance_t> > s_inst;   // keyed by entity / test token
    const int   s_testToken = 0;
    int         s_mode = KFXP_SELECTED;
    bool        s_paused = false;
    float       s_timeScale = 1.0f;
    bool        s_loaded = false;
    LARGE_INTEGER s_lastTick = { 0 };
    int         s_liveParts = 0;
    int         s_drawnQuads = 0;

    void LoadPrefs()
    {
        if ( s_loaded )
            return;
        s_loaded = true;
        s_mode = Radiant_ProfileGetInt( KFXP_PROFILE, "Mode", KFXP_SELECTED );
        if ( s_mode < KFXP_OFF || s_mode > KFXP_ALL )
            s_mode = KFXP_SELECTED;
    }

    int CountParts( const instance_t &in )
    {
        int n = (int)in.parts.size();
        for ( size_t c = 0; c < in.children.size(); ++c )
            n += CountParts( *in.children[c] );
        return n;
    }

    void ResetInstance( instance_t &in )
    {
        in.parts.clear();
        in.children.clear();
        in.loopSpawned.clear();
        in.oneShotDone = false;
        in.idleSince = -1;
        in.msecSpawned = -1;
        in.msecNow = 0.0;
        in.frameAtStart = in.frame;
        in.seed = ++s_seedCounter * 2654435761u;
    }

    void SpawnRunner( instance_t &in, const elemRt_t &rt, const float pos[3], const particle_t &p, int msecBegin );

    // FX_SpawnElem + FX_GetOriginForElem for one particle born at msecBegin.
    void SpawnElem( instance_t &in, int elemIndex, const elemRt_t &rt, int msecWhen, int sequence )
    {
        const kefxElem_t &e = rt.src;
        rng_t rng( in.seed ^ ( (unsigned)elemIndex * 0x85EBCA6Bu ) ^ ( (unsigned)sequence * 0xC2B2AE35u )
                   ^ ( (unsigned)msecWhen * 0x27D4EB2Fu ) );
        particle_t p;
        p.elem = elemIndex;
        p.sequence = sequence;
        for ( int k = 0; k < R_COUNT; ++k )
            p.rnd[k] = rng.Next();
        p.msecBegin = msecWhen + IntRand( e.spawnDelayMsec, rng.Next() );
        p.life = ( std::max )( 1, IntRand( e.lifeSpanMsec, rng.Next() ) );

        // Spawn origin in the frame the effect had when this element was played.
        const frame_t &when = in.frame;
        float off[3];
        for ( int a = 0; a < 3; ++a )
            off[a] = Lerp( e.spawnOrg[a], p.rnd[R_ORGX + a] );
        float spawn[3];
        if ( e.flags & KEFX_F_SPAWNRELATIVE )
            LocalToWorld( when, off, spawn );
        else
            for ( int a = 0; a < 3; ++a ) spawn[a] = when.org[a] + off[a];
        float offset[3] = { 0, 0, 0 };
        const unsigned offMode = e.flags & KEFX_F_OFFSET_MASK;
        const float radius = Lerp( e.spawnOffsetRadius, p.rnd[R_RADIUS] );
        if ( offMode == KEFX_F_OFFSET_SPHERE )
        {
            // FX_RandomDir: a uniform direction.
            const float z = rng.Next() * 2.0f - 1.0f, th = rng.Next() * 2.0f * KFXP_PI, s = sqrtf( ( std::max )( 0.0f, 1.0f - z * z ) );
            const float dir[3] = { s * cosf( th ), s * sinf( th ), z };
            for ( int a = 0; a < 3; ++a ) offset[a] = dir[a] * radius;
        }
        else if ( offMode == KEFX_F_OFFSET_CYLINDER )
        {
            const float yaw = p.rnd[R_YAW] * 2.0f * KFXP_PI;
            const float h = Lerp( e.spawnOffsetHeight, p.rnd[R_HEIGHT] );
            for ( int a = 0; a < 3; ++a )
                offset[a] = radius * cosf( yaw ) * when.axis[1][a] + radius * sinf( yaw ) * when.axis[2][a] + h * when.axis[0][a];
        }

        if ( e.type == KEFX_RUNNER )
        {
            float pos[3] = { spawn[0] + offset[0], spawn[1] + offset[1], spawn[2] + offset[2] };
            SpawnRunner( in, rt, pos, p, p.msecBegin );
            return;
        }
        if ( !rt.drawable )
            return;                              // models, lights, sounds, decals, trails: not previewed
        if ( p.msecBegin + p.life <= (int)in.msecNow )
            return;                              // already over (prewarm)
        if ( s_liveParts >= KFXP_MAX_PARTS )
            return;

        const unsigned run = e.flags & KEFX_F_RUN_MASK;
        if ( run == KEFX_F_RUN_OFFSET )
        {
            // FX_GetOrientation, run relative to offset: origin = spawn + offset, forward = offset dir.
            for ( int a = 0; a < 3; ++a )
            {
                p.orient.org[a] = spawn[a] + offset[a];
                p.orient.axis[0][a] = offset[a];
            }
            Normalize( p.orient.axis[0] );
            if ( fabsf( p.orient.axis[0][0] ) + fabsf( p.orient.axis[0][1] ) + fabsf( p.orient.axis[0][2] ) < 1e-6f )
            {
                p.orient.axis[0][0] = 1.0f; p.orient.axis[0][1] = 0.0f; p.orient.axis[0][2] = 0.0f;
            }
            float up[3] = { 0.0f, 0.0f, 1.0f };
            if ( fabsf( p.orient.axis[0][2] ) >= 0.999f ) { up[1] = 1.0f; up[2] = 0.0f; }
            Cross( up, p.orient.axis[0], p.orient.axis[1] );
            Normalize( p.orient.axis[1] );
            Cross( p.orient.axis[0], p.orient.axis[1], p.orient.axis[2] );
            p.origin[0] = p.origin[1] = p.origin[2] = 0.0f;
        }
        else
        {
            float world[3] = { spawn[0] + offset[0], spawn[1] + offset[1], spawn[2] + offset[2] };
            if ( run == KEFX_F_RUN_SPAWN )
            {
                p.orient = in.frameAtStart;
                WorldToLocal( in.frameAtStart, world, p.origin );
            }
            else if ( run == KEFX_F_RUN_EFFECT )
            {
                p.orient = when;
                WorldToLocal( when, world, p.origin );
            }
            else
                memcpy( p.origin, world, sizeof( world ) );
        }
        in.parts.push_back( p );
        ++s_liveParts;
    }

    void SpawnRunner( instance_t &in, const elemRt_t &rt, const float pos[3], const particle_t &p, int msecBegin )
    {
        if ( in.depth + 1 > KFXP_MAX_DEPTH || (int)in.children.size() >= KFXP_MAX_CHILDREN || rt.src.visuals.empty() )
            return;
        const kefxElem_t &e = rt.src;
        const size_t pick = (size_t)( p.rnd[R_ATLAS] * (float)e.visuals.size() ) % e.visuals.size();
        std::unique_ptr<instance_t> child( new instance_t() );
        child->fx = e.visuals[pick];
        child->depth = in.depth + 1;
        child->repeat = false;
        // FX_SpawnRunner: the effect frame, turned by the element's spawn angles.
        float ang[3];
        for ( int a = 0; a < 3; ++a )
            ang[a] = Lerp( e.spawnAngles[a], p.rnd[R_ANG0 + a] );
        frame_t turn;
        const float zero[3] = { 0, 0, 0 };
        AnglesToFrame( ang, zero, turn );
        for ( int r = 0; r < 3; ++r )
            for ( int k = 0; k < 3; ++k )
                child->frame.axis[r][k] = turn.axis[r][0] * in.frame.axis[0][k] + turn.axis[r][1] * in.frame.axis[1][k]
                                        + turn.axis[r][2] * in.frame.axis[2][k];
        memcpy( child->frame.org, pos, sizeof( child->frame.org ) );
        ResetInstance( *child );
        // The child starts at the runner's birth time, on the parent's clock.
        child->msecNow = in.msecNow - (double)msecBegin;
        if ( child->msecNow < 0.0 )
        {
            child->msecSpawned = (int)child->msecNow - 1;   // waits for its delay
        }
        in.children.push_back( std::move( child ) );
    }

    // Spawn everything due in (msecSpawned, upTo] on the instance clock.
    void RunSpawns( instance_t &in, defRt_t &def, int upTo )
    {
        if ( in.loopSpawned.size() != def.elems.size() )
            in.loopSpawned.assign( def.elems.size(), 0 );
        if ( upTo < 0 )
            return;
        const bool first = in.msecSpawned < 0;
        for ( size_t i = 0; i < def.elems.size(); ++i )
        {
            const elemRt_t &rt = def.elems[i];
            const kefxElem_t &e = rt.src;
            if ( e.Looping() )
            {
                const int interval = ( std::max )( 1, e.loopIntervalMsec );
                const int count = e.loopCount > 0 ? e.loopCount : 0x7FFFFFFF;
                int &done = in.loopSpawned[i];
                while ( done < count )
                {
                    const int when = done * interval;
                    if ( when > upTo )
                        break;
                    SpawnElem( in, (int)i, rt, when, done );
                    ++done;
                }
            }
            else if ( first )
            {
                // FX_SpawnOneShotElems: base + ((amp + 1) * r) >> 16 copies.
                rng_t rng( in.seed ^ ( 0xA511E9B3u * ( (unsigned)i + 1 ) ) );
                const int n = ( std::min )( 256, IntRand( e.oneShotCount, rng.Next() ) );
                for ( int k = 0; k < n; ++k )
                    SpawnElem( in, (int)i, rt, 0, k );
            }
        }
        in.msecSpawned = upTo;
        in.oneShotDone = true;
    }

    // Keeps live particles, drops finished ones, advances children.
    void Advance( instance_t &in, double dtMsec )
    {
        defRt_t *def = Def( in.fx );
        if ( !def || !def->ok )
            return;
        in.msecNow += dtMsec;
        const int now = (int)in.msecNow;
        if ( now >= 0 )
            RunSpawns( in, *def, now );
        size_t w = 0;
        for ( size_t i = 0; i < in.parts.size(); ++i )
        {
            const particle_t &p = in.parts[i];
            if ( p.elem < (int)def->elems.size() && p.msecBegin + p.life > now )
                in.parts[w++] = p;
        }
        in.parts.resize( w );
        size_t cw = 0;
        for ( size_t c = 0; c < in.children.size(); ++c )
        {
            instance_t &ch = *in.children[c];
            Advance( ch, dtMsec );
            // A looping child plays as long as its parent does (an ambient runner's loops);
            // a one-shot child goes once its particles are gone.
            defRt_t *cd = Def( ch.fx );
            const bool alive = cd && cd->ok && ( cd->looping || !ch.parts.empty() || !ch.children.empty()
                                                 || ch.msecNow < (double)cd->maxLifeMsec );
            if ( alive )
                in.children[cw++] = std::move( in.children[c] );
        }
        in.children.resize( cw );

        // A finished one-shot effect replays after a pause (top-level instances only).
        if ( in.repeat && !def->looping && in.parts.empty() && in.children.empty() && now > def->maxLifeMsec )
        {
            if ( in.idleSince < 0 )
                in.idleSince = now;
            else if ( now - in.idleSince >= KFXP_REPEAT_PAUSE )
                ResetInstance( in );
        }
    }

    void Recount();

    void StartInstance( instance_t &in )
    {
        ResetInstance( in );
        Recount();
        defRt_t *def = Def( in.fx );
        if ( def && def->ok && def->looping )
        {
            // Looping effects show their steady state at once (createfx's negative delay).
            const int prewarm = ( std::min )( KFXP_PREWARM_MAX, def->maxLifeMsec );
            if ( prewarm > 0 )
                Advance( in, (double)prewarm );
        }
    }

    // ── drawing ──────────────────────────────────────────────────────────────
    struct quad_t
    {
        Material *mat;
        float     dist;
        int       sortOrder;
        float     xyz[4][3];
        float     st[4][2];
        unsigned  color;
        bool      twoSided;
    };
    std::vector<quad_t> s_quads;

    void ParticlePos( const instance_t &in, const elemRt_t &rt, const particle_t &p, float tn, float out[3] )
    {
        const float lifeSec = (float)p.life * 0.001f;
        const float x = tn * (float)KFXP_SAMPLES;
        int i0 = (int)floorf( x );
        if ( i0 < 0 ) i0 = 0;
        if ( i0 >= KFXP_SAMPLES ) i0 = KFXP_SAMPLES - 1;
        const float f = x - (float)i0;
        float local[3], world[3] = { 0, 0, 0 };
        for ( int a = 0; a < 3; ++a )
        {
            local[a] = p.origin[a];
            const float r = p.rnd[R_VX + a];
            if ( rt.hasLoc )
            {
                const float b = rt.locBase[i0][a] + ( rt.locBase[i0 + 1][a] - rt.locBase[i0][a] ) * f;
                const float am = rt.locAmp[i0][a] + ( rt.locAmp[i0 + 1][a] - rt.locAmp[i0][a] ) * f;
                local[a] += ( b + r * am ) * lifeSec;
            }
            if ( rt.hasWld )
            {
                const float b = rt.wldBase[i0][a] + ( rt.wldBase[i0 + 1][a] - rt.wldBase[i0][a] ) * f;
                const float am = rt.wldAmp[i0][a] + ( rt.wldAmp[i0 + 1][a] - rt.wldAmp[i0][a] ) * f;
                world[a] = ( b + r * am ) * lifeSec;
            }
        }
        const unsigned run = rt.src.flags & KEFX_F_RUN_MASK;
        if ( run == 0 )
            memcpy( out, local, sizeof( local ) );                   // world run: identity orientation
        else if ( run == KEFX_F_RUN_EFFECT )
            LocalToWorld( in.frame, local, out );                    // follows the effect as it moves
        else
            LocalToWorld( p.orient, local, out );
        for ( int a = 0; a < 3; ++a )
            out[a] += world[a];
        const float g = Lerp( rt.src.gravity, p.rnd[R_GRAV] ) * 0.01f * 800.0f;
        const float tSec = tn * lifeSec;
        out[2] -= 0.5f * g * tSec * tSec;
    }

    // Velocity direction for tails (FX_GetVelocityAtTime): finite difference of the path.
    void ParticleDir( const instance_t &in, const elemRt_t &rt, const particle_t &p, float tn, const float pos[3], float dir[3] )
    {
        const float dt = 0.01f;
        float a[3];
        if ( tn + dt <= 1.0f )
        {
            ParticlePos( in, rt, p, tn + dt, a );
            for ( int k = 0; k < 3; ++k ) dir[k] = a[k] - pos[k];
        }
        else
        {
            ParticlePos( in, rt, p, tn - dt, a );
            for ( int k = 0; k < 3; ++k ) dir[k] = pos[k] - a[k];
        }
        Normalize( dir );
    }

    void EmitParticle( const instance_t &in, const elemRt_t &rt, const particle_t &p, const camera_s *cam )
    {
        const kefxElem_t &e = rt.src;
        const float age = (float)( in.msecNow - (double)p.msecBegin );
        if ( age < 0.0f )
            return;
        float tn = age / (float)p.life;
        if ( tn > 1.0f ) tn = 1.0f;
        float pos[3];
        ParticlePos( in, rt, p, tn, pos );

        // FX_SampleVisualState / FX_EvaluateVisualState
        const float cr = p.rnd[R_COLOR];
        const int colorSrc = ( e.editorFlags & KEFX_E_RANDCOLOR ) ? 1 : 0;
        const int alphaSrc = ( e.editorFlags & KEFX_E_RANDALPHA ) ? 1 : 0;
        float rgba[4];
        for ( int k = 0; k < 3; ++k )
        {
            const float a0 = KiwiEfx_Sample( e.color.curve[0], 3, tn, k ), a1 = KiwiEfx_Sample( e.color.curve[colorSrc], 3, tn, k );
            rgba[k] = a0 + ( a1 - a0 ) * cr;
        }
        {
            const float a0 = KiwiEfx_Sample( e.alpha.curve[0], 1, tn ), a1 = KiwiEfx_Sample( e.alpha.curve[alphaSrc], 1, tn );
            rgba[3] = a0 + ( a1 - a0 ) * cr;
        }
        if ( e.editorFlags & KEFX_E_MODCOLORBYALPHA )
        {
            for ( int k = 0; k < 3; ++k ) rgba[k] *= rgba[3];
            rgba[3] = 1.0f;
        }
        // FX_EvaluateDistanceFade
        const float dx = pos[0] - cam->origin[0], dy = pos[1] - cam->origin[1], dz = pos[2] - cam->origin[2];
        const float dist = sqrtf( dx * dx + dy * dy + dz * dz );
        if ( e.fadeInRange.amp != 0.0f || e.fadeOutRange.amp != 0.0f )
        {
            auto clampLerp = []( float d, const kefxRange_t &r ) -> float
            {
                const float b = d - r.base;
                if ( b < 0.0f ) return 1.0f;
                return r.amp > b ? 1.0f - b / r.amp : 0.0f;
            };
            float fin = 1.0f, fout = 1.0f;
            if ( e.fadeInRange.amp != 0.0f ) fin = clampLerp( dist, e.fadeInRange );
            if ( e.fadeOutRange.amp != 0.0f ) fout = 1.0f - clampLerp( dist, e.fadeOutRange );
            rgba[3] *= ( std::min )( fin, fout );
        }
        if ( rgba[3] <= 0.004f && !( e.editorFlags & KEFX_E_MODCOLORBYALPHA ) )
            return;
        for ( int k = 0; k < 4; ++k ) rgba[k] = rgba[k] < 0.0f ? 0.0f : ( rgba[k] > 1.0f ? 1.0f : rgba[k] );

        // Sizes: 0.5 * scale * curve, randomised between the two curves.
        auto sizeOf = [&]( int s, unsigned randFlag, int rkey ) -> float
        {
            const float c0 = KiwiEfx_Sample( e.size[s].curve[0], 1, tn );
            float v = c0;
            if ( e.editorFlags & randFlag )
                v = c0 + ( KiwiEfx_Sample( e.size[s].curve[1], 1, tn ) - c0 ) * p.rnd[rkey];
            return 0.5f * e.size[s].scale * v;
        };
        float s0 = sizeOf( 0, KEFX_E_RANDSIZE0, R_SIZE0 );
        float s1 = ( e.flags & KEFX_F_NONUNIFORMSCALE ) ? sizeOf( 1, KEFX_E_RANDSIZE1, R_SIZE1 ) : s0;
        if ( e.type == KEFX_CLOUD )
        {
            // Clouds route size1 always and scale the puff by the scale graph.
            s1 = sizeOf( 1, KEFX_E_RANDSIZE1, R_SIZE1 );
            const float c0 = KiwiEfx_Sample( e.scale.curve[0], 1, tn );
            float sc = c0;
            if ( e.editorFlags & KEFX_E_RANDSCALE )
                sc = c0 + ( KiwiEfx_Sample( e.scale.curve[1], 1, tn ) - c0 ) * p.rnd[R_SCALE];
            sc *= e.scale.scale;
            if ( sc > 0.0f ) { s0 *= sc; s1 *= sc; }
        }
        if ( s0 <= 0.01f && s1 <= 0.01f )
            return;

        // Rotation: initial + integrated rotation graph.
        const float lifeSec = (float)p.life * 0.001f;
        const float x = tn * (float)KFXP_SAMPLES;
        int i0 = (int)floorf( x );
        if ( i0 >= KFXP_SAMPLES ) i0 = KFXP_SAMPLES - 1;
        const float f = x - (float)i0;
        const float rb = rt.rotBase[i0] + ( rt.rotBase[i0 + 1] - rt.rotBase[i0] ) * f;
        const float ra = rt.rotAmp[i0] + ( rt.rotAmp[i0 + 1] - rt.rotAmp[i0] ) * f;
        const float rot = Lerp( e.initialRot, p.rnd[R_COLOR] ) * KFXP_PI / 180.0f + ( rb + ra * p.rnd[R_ROT] ) * lifeSec;

        // Axes of the quad.
        float right[3], up[3], center[3] = { pos[0], pos[1], pos[2] };
        bool twoSided = false;
        if ( e.type == KEFX_TAIL )
        {
            float dir[3], toCam[3], tan[3];
            ParticleDir( in, rt, p, tn, pos, dir );
            for ( int k = 0; k < 3; ++k ) center[k] = pos[k] - dir[k] * s1;
            for ( int k = 0; k < 3; ++k ) toCam[k] = cam->origin[k] - center[k];
            Cross( dir, toCam, tan );
            Normalize( tan );
            memcpy( right, tan, sizeof( right ) );
            memcpy( up, dir, sizeof( up ) );
        }
        else
        {
            float baseR[3], baseU[3];
            if ( e.type == KEFX_ORIENTED )
            {
                // The run frame turned by the spawn angles + angular velocity.
                frame_t runFrame;
                const unsigned run = e.flags & KEFX_F_RUN_MASK;
                if ( run == KEFX_F_RUN_EFFECT ) runFrame = in.frame;
                else if ( run == 0 ) runFrame = in.frameAtStart;
                else runFrame = p.orient;
                float ang[3];
                const float ageSec = age * 0.001f;
                for ( int a = 0; a < 3; ++a )
                    ang[a] = Lerp( e.spawnAngles[a], p.rnd[R_ANG0 + a] ) + Lerp( e.angleVel[a], p.rnd[R_AVEL0 + a] ) * ageSec;
                frame_t turn;
                const float zero[3] = { 0, 0, 0 };
                AnglesToFrame( ang, zero, turn );
                float ax[3][3];
                for ( int r = 0; r < 3; ++r )
                    for ( int k = 0; k < 3; ++k )
                        ax[r][k] = turn.axis[r][0] * runFrame.axis[0][k] + turn.axis[r][1] * runFrame.axis[1][k]
                                 + turn.axis[r][2] * runFrame.axis[2][k];
                // FX_DrawElem_OrientedSprite: tangent = axis[1], binormal = axis[2], normal = axis[0].
                for ( int k = 0; k < 3; ++k ) { baseR[k] = -ax[1][k]; baseU[k] = ax[2][k]; }
                twoSided = true;
            }
            else
            {
                memcpy( baseR, cam->vright, sizeof( baseR ) );
                memcpy( baseU, cam->vup, sizeof( baseU ) );
            }
            const float c = cosf( rot ), s = sinf( rot );
            for ( int k = 0; k < 3; ++k )
            {
                right[k] = baseR[k] * c + baseU[k] * s;
                up[k]    = baseU[k] * c - baseR[k] * s;
            }
        }

        // FX_GetSpriteTexCoords
        float sS = 0.0f, dS = 1.0f, sT = 0.0f, dT = 1.0f;
        if ( rt.entryCount > 1 )
        {
            int index;
            const unsigned start = e.atlasBehavior & KEFX_A_START_MASK;
            if ( start == 1 )      index = (int)( p.rnd[R_ATLAS] * (float)rt.entryCount );
            else if ( start == 2 ) index = ( rt.entryCount - 1 ) & p.sequence;
            else                   index = e.atlasIndex;
            if ( e.atlasBehavior & KEFX_A_PLAYOVERLIFE )
                index += (int)( (float)rt.entryCount * tn );
            else if ( e.atlasFps )
                index += e.atlasFps * (int)age / 1000;
            if ( ( e.atlasBehavior & KEFX_A_LOOPONLYN ) && index >= rt.entryCount * ( e.atlasLoopCount + 1 ) )
                index = rt.entryCount - 1;
            index &= rt.entryCount - 1;
            dS = 1.0f / (float)( 1 << rt.colBits );
            sS = (float)( index & ( ( 1 << rt.colBits ) - 1 ) ) * dS;
            dT = 1.0f / (float)( 1 << rt.rowBits );
            sT = (float)( index >> rt.colBits ) * dT;
        }

        const size_t vis = rt.draw.size() > 1 ? (size_t)( p.rnd[R_ATLAS] * 7919.0f ) % rt.draw.size() : 0;
        Material *mat = rt.draw.empty() ? nullptr : rt.draw[vis];
        if ( !mat )
            return;
        quad_t q;
        q.mat = mat;
        q.dist = dist;
        q.sortOrder = e.sortOrder;
        q.twoSided = twoSided;
        for ( int k = 0; k < 3; ++k )
        {
            q.xyz[0][k] = center[k] - right[k] * s0 + up[k] * s1;
            q.xyz[1][k] = center[k] + right[k] * s0 + up[k] * s1;
            q.xyz[2][k] = center[k] + right[k] * s0 - up[k] * s1;
            q.xyz[3][k] = center[k] - right[k] * s0 - up[k] * s1;
        }
        q.st[0][0] = sS;      q.st[0][1] = sT;
        q.st[1][0] = sS + dS; q.st[1][1] = sT;
        q.st[2][0] = sS + dS; q.st[2][1] = sT + dT;
        q.st[3][0] = sS;      q.st[3][1] = sT + dT;
        GfxColor packed;
        Byte4PackPixelColor( rgba, &packed );
        q.color = packed.packed;
        s_quads.push_back( q );
    }

    void EmitInstance( const instance_t &in, const camera_s *cam )
    {
        defRt_t *def = Def( in.fx );
        if ( def && def->ok )
            for ( size_t i = 0; i < in.parts.size(); ++i )
            {
                const particle_t &p = in.parts[i];
                if ( p.elem < (int)def->elems.size() )
                    EmitParticle( in, def->elems[p.elem], p, cam );
            }
        for ( size_t c = 0; c < in.children.size(); ++c )
            EmitInstance( *in.children[c], cam );
    }

    bool QuadFarFirst( const quad_t &a, const quad_t &b )
    {
        if ( a.sortOrder != b.sortOrder )
            return a.sortOrder > b.sortOrder;    // higher sort orders first, like the game's draw lists
        return a.dist > b.dist;
    }

    void FlushQuads()
    {
        s_drawnQuads = 0;
        if ( s_quads.empty() )
            return;
        std::stable_sort( s_quads.begin(), s_quads.end(), QuadFarFirst );
        const int BATCH = 512;
        static float    xyzw[BATCH * 4][4];
        static float    normal[BATCH * 4][3];
        static float    color[BATCH * 4];
        static float    st[BATCH * 4][2];
        static uint16_t index[BATCH * 12];
        // The twins draw like reference images: neutral MATERIAL_COLOR = the vertex colour.
        KiwiTris_FillNeutral();
        size_t i = 0;
        while ( i < s_quads.size() )
        {
            Material *mat = s_quads[i].mat;
            int n = 0, idx = 0;
            const int room = R_Ed_CmdBufferHeadroom() - 4096;
            const int maxQuads = ( std::min )( BATCH, room / KFXP_QUAD_BYTES );
            if ( maxQuads <= 0 )
                break;
            while ( i < s_quads.size() && s_quads[i].mat == mat && n < maxQuads )
            {
                const quad_t &q = s_quads[i];
                float nrm[3], e1[3], e2[3];
                for ( int k = 0; k < 3; ++k ) { e1[k] = q.xyz[1][k] - q.xyz[0][k]; e2[k] = q.xyz[3][k] - q.xyz[0][k]; }
                Cross( e1, e2, nrm );
                Normalize( nrm );
                float colorF;
                memcpy( &colorF, &q.color, sizeof( colorF ) );
                for ( int v = 0; v < 4; ++v )
                {
                    const int o = n * 4 + v;
                    xyzw[o][0] = q.xyz[v][0]; xyzw[o][1] = q.xyz[v][1]; xyzw[o][2] = q.xyz[v][2]; xyzw[o][3] = 1.0f;
                    normal[o][0] = nrm[0]; normal[o][1] = nrm[1]; normal[o][2] = nrm[2];
                    color[o] = colorF;
                    st[o][0] = q.st[v][0]; st[o][1] = q.st[v][1];
                }
                const uint16_t b = (uint16_t)( n * 4 );
                index[idx++] = b; index[idx++] = (uint16_t)( b + 1 ); index[idx++] = (uint16_t)( b + 2 );
                index[idx++] = b; index[idx++] = (uint16_t)( b + 2 ); index[idx++] = (uint16_t)( b + 3 );
                if ( q.twoSided )
                {
                    index[idx++] = b; index[idx++] = (uint16_t)( b + 2 ); index[idx++] = (uint16_t)( b + 1 );
                    index[idx++] = b; index[idx++] = (uint16_t)( b + 3 ); index[idx++] = (uint16_t)( b + 2 );
                }
                ++n;
                ++i;
            }
            R_AddRenderCmdDrawTris( mat, TECHNIQUE_UNLIT, (short)idx, index,
                                    (short)( n * 4 ), xyzw, normal, color, st );
            s_drawnQuads += n;
        }
        s_quads.clear();
    }

    double TickMsec()
    {
        LARGE_INTEGER now, freq;
        ::QueryPerformanceCounter( &now );
        ::QueryPerformanceFrequency( &freq );
        if ( s_lastTick.QuadPart == 0 || freq.QuadPart == 0 )
        {
            s_lastTick = now;
            return 0.0;
        }
        double ms = (double)( now.QuadPart - s_lastTick.QuadPart ) * 1000.0 / (double)freq.QuadPart;
        s_lastTick = now;
        if ( ms > 100.0 ) ms = 100.0;            // a stalled frame does not fast-forward the effect
        if ( ms < 0.0 ) ms = 0.0;
        return ms;
    }

    void SyncFromEntities()
    {
        std::vector<kiwiFxEnt_t> ents;
        if ( s_mode != KFXP_OFF )
            KiwiParticles_Collect( ents, s_mode == KFXP_SELECTED );
        const camera_s *cam = Ed_Camera();
        for ( std::map<const void *, std::unique_ptr<instance_t> >::iterator it = s_inst.begin(); it != s_inst.end(); ++it )
            if ( it->first != &s_testToken )
                it->second->seen = false;
        for ( size_t i = 0; i < ents.size(); ++i )
        {
            const kiwiFxEnt_t &e = ents[i];
            if ( e.fx.empty() )
                continue;
            if ( s_mode == KFXP_ALL && !e.selected && cam )
            {
                const float dx = e.origin[0] - cam->origin[0], dy = e.origin[1] - cam->origin[1], dz = e.origin[2] - cam->origin[2];
                if ( dx * dx + dy * dy + dz * dz > KFXP_ALL_RADIUS * KFXP_ALL_RADIUS )
                    continue;
            }
            std::unique_ptr<instance_t> &slot = s_inst[e.key];
            const bool fresh = !slot;
            if ( fresh )
                slot.reset( new instance_t() );
            instance_t &in = *slot;
            in.seen = true;
            AnglesToFrame( e.angles, e.origin, in.frame );
            if ( fresh || Key( in.fx.c_str() ) != Key( e.fx.c_str() ) )
            {
                in.fx = e.fx;
                StartInstance( in );
            }
        }
        for ( std::map<const void *, std::unique_ptr<instance_t> >::iterator it = s_inst.begin(); it != s_inst.end(); )
        {
            if ( !it->second->seen )
                it = s_inst.erase( it );
            else
                ++it;
        }
    }

    // The particle cap's counter: recounted before every update, SpawnElem adds to it.
    void Recount()
    {
        s_liveParts = 0;
        for ( std::map<const void *, std::unique_ptr<instance_t> >::const_iterator it = s_inst.begin(); it != s_inst.end(); ++it )
            s_liveParts += CountParts( *it->second );
    }
}

// ── public ─────────────────────────────────────────────────────────────────────
void KiwiFxPreview_DrawWorld()
{
    LoadPrefs();
    SyncFromEntities();
    const double dt = TickMsec();
    if ( s_inst.empty() )
    {
        s_drawnQuads = 0;
        return;
    }
    camera_s *cam = Ed_Camera();
    if ( !cam )
        return;
    Recount();
    if ( !s_paused )
        for ( std::map<const void *, std::unique_ptr<instance_t> >::iterator it = s_inst.begin(); it != s_inst.end(); ++it )
            Advance( *it->second, dt * (double)s_timeScale );
    s_quads.clear();
    for ( std::map<const void *, std::unique_ptr<instance_t> >::iterator it = s_inst.begin(); it != s_inst.end(); ++it )
        EmitInstance( *it->second, cam );
    FlushQuads();                                // the camera renders every tick: no redraw request needed
}

void KiwiFxPreview_ResetForNewMap()
{
    s_inst.clear();
    s_liveParts = 0;
    s_drawnQuads = 0;
}

int KiwiFxPreview_Mode()
{
    LoadPrefs();
    return s_mode;
}

void KiwiFxPreview_SetMode( int mode )
{
    LoadPrefs();
    if ( mode < KFXP_OFF || mode > KFXP_ALL )
        return;
    s_mode = mode;
    Radiant_ProfileSetInt( KFXP_PROFILE, "Mode", mode );
    g_nUpdateBits |= W_CAMERA;
}

bool KiwiFxPreview_Paused() { return s_paused; }
void KiwiFxPreview_SetPaused( bool paused )
{
    s_paused = paused;
    g_nUpdateBits |= W_CAMERA;
}

float KiwiFxPreview_TimeScale() { return s_timeScale; }
void KiwiFxPreview_SetTimeScale( float scale )
{
    s_timeScale = scale < 0.05f ? 0.05f : ( scale > 4.0f ? 4.0f : scale );
}

void KiwiFxPreview_SetOverride( const char *fxName, const kefxDoc_t *doc )
{
    const std::string key = Key( fxName );
    if ( key.empty() )
        return;
    if ( doc )
    {
        s_override[key] = *doc;
        std::unique_ptr<defRt_t> &slot = s_defs[key];
        if ( !slot )
            slot.reset( new defRt_t() );
        slot->name = fxName;
        slot->stamp = 0;
        slot->err.clear();
        Compile( *slot, *doc );
    }
    else
    {
        s_override.erase( key );
        s_defs.erase( key );
    }
    KiwiFxPreview_Restart( fxName );
}

void KiwiFxPreview_Invalidate( const char *fxName )
{
    const std::string key = Key( fxName );
    if ( s_override.find( key ) == s_override.end() )
        s_defs.erase( key );
    KiwiFxPreview_Restart( fxName );
}

void KiwiFxPreview_Restart( const char *fxName )
{
    const std::string key = fxName ? Key( fxName ) : std::string();
    // Children may play the effect too: restart any top-level instance that is or contains it.
    for ( std::map<const void *, std::unique_ptr<instance_t> >::iterator it = s_inst.begin(); it != s_inst.end(); ++it )
    {
        instance_t &in = *it->second;
        if ( !key.empty() && Key( in.fx.c_str() ) != key && in.children.empty() )
            continue;
        StartInstance( in );
    }
    g_nUpdateBits |= W_CAMERA;
}

void KiwiFxPreview_PlayTest( const char *fxName, const float origin[3], const float forward[3] )
{
    if ( !fxName || !fxName[0] )
        return;
    std::unique_ptr<instance_t> &slot = s_inst[&s_testToken];
    slot.reset( new instance_t() );
    instance_t &in = *slot;
    in.fx = fxName;
    // Forward -> angles (pitch, yaw) like a kiwi_fx "angles" key.
    float f[3] = { forward[0], forward[1], forward[2] };
    Normalize( f );
    const float flat = sqrtf( f[0] * f[0] + f[1] * f[1] );
    float ang[3] = { atan2f( -f[2], flat ) * 180.0f / KFXP_PI, flat > 1e-6f ? atan2f( f[1], f[0] ) * 180.0f / KFXP_PI : 0.0f, 0.0f };
    AnglesToFrame( ang, origin, in.frame );
    StartInstance( in );
    g_nUpdateBits |= W_CAMERA;
}

void KiwiFxPreview_StopTest()
{
    std::map<const void *, std::unique_ptr<instance_t> >::iterator it = s_inst.find( &s_testToken );
    if ( it == s_inst.end() )
        return;
    s_inst.erase( it );
    g_nUpdateBits |= W_CAMERA;
}

bool KiwiFxPreview_TestPlaying()
{
    return s_inst.find( &s_testToken ) != s_inst.end();
}

int KiwiFxPreview_ActiveInstances() { return (int)s_inst.size(); }
int KiwiFxPreview_LiveParticles()
{
    int n = 0;
    for ( std::map<const void *, std::unique_ptr<instance_t> >::const_iterator it = s_inst.begin(); it != s_inst.end(); ++it )
        n += CountParts( *it->second );
    return n;
}
int KiwiFxPreview_DrawnQuads() { return s_drawnQuads; }

std::string KiwiFxPreview_MissingMaterials( const char *fxName )
{
    defRt_t *d = Def( fxName ? fxName : "" );
    if ( !d )
        return std::string();
    std::set<std::string> names;
    for ( size_t i = 0; i < d->elems.size(); ++i )
        names.insert( d->elems[i].missing.begin(), d->elems[i].missing.end() );
    std::string out;
    for ( std::set<std::string>::const_iterator it = names.begin(); it != names.end(); ++it )
        out += ( out.empty() ? "" : ", " ) + *it;
    return out;
}

std::string KiwiFxPreview_Describe( const char *fxName )
{
    std::string out;
    char buf[512];
    _snprintf( buf, sizeof( buf ), "instances %i particles %i quads(last frame) %i",
               KiwiFxPreview_ActiveInstances(), KiwiFxPreview_LiveParticles(), s_drawnQuads );
    buf[sizeof( buf ) - 1] = '\0';
    out = buf;
    defRt_t *d = Def( fxName ? fxName : "" );
    if ( !d )
        return out;
    if ( !d->ok )
        return out + " | not loaded: " + d->err;
    for ( size_t i = 0; i < d->elems.size(); ++i )
    {
        const elemRt_t &rt = d->elems[i];
        for ( size_t v = 0; v < rt.mats.size(); ++v )
        {
            Material *m = rt.mats[v];
            const Material *info = m ? Material_FromHandle( m ) : nullptr;
            const MaterialTechniqueSet *ts = info ? info->techniqueSet : nullptr;
            const MaterialTechniqueSet *rs = ts ? ts->remappedTechniqueSet : nullptr;
            _snprintf( buf, sizeof( buf ), " | %s: %s techset %s -> %s unlit %i twin %i atlas %ix%i",
                       rt.src.name.c_str(), rt.src.visuals[v].c_str(), ts && ts->name ? ts->name : "-",
                       rs && rs->name ? rs->name : "-", m && Material_GetTechnique( m, TECHNIQUE_UNLIT ) ? 1 : 0,
                       v < rt.draw.size() && rt.draw[v] ? 1 : 0, 1 << rt.colBits, 1 << rt.rowBits );
            buf[sizeof( buf ) - 1] = '\0';
            out += buf;
            for ( int tech = TECHNIQUE_UNLIT; info && tech <= TECHNIQUE_EMISSIVE; ++tech )
            {
                const uint8_t e = info->stateBitsEntry[tech];
                const MaterialTechnique *mt = Material_GetTechnique( m, (MaterialTechniqueType)tech );
                _snprintf( buf, sizeof( buf ), " [%s %s entry %i bits %08x %08x]", tech == TECHNIQUE_UNLIT ? "unlit" : "emissive",
                           mt && mt->name ? mt->name : "-", (int)e,
                           e != 0xFF && info->stateBitsTable ? info->stateBitsTable[e].loadBits[0] : 0u,
                           e != 0xFF && info->stateBitsTable ? info->stateBitsTable[e].loadBits[1] : 0u );
                buf[sizeof( buf ) - 1] = '\0';
                out += buf;
            }
            for ( int t = 0; info && t < info->textureCount && info->textureTable; ++t )
            {
                const GfxImage *img = info->textureTable[t].u.image;
                if ( !img || info->textureTable[t].semantic == 11 )      // 11 = water
                    continue;
                _snprintf( buf, sizeof( buf ), " [img %s %ix%i tex %i delay %i]", img->name ? img->name : "?",
                           img->width, img->height, img->texture.basemap ? 1 : 0, img->delayLoadPixels ? 1 : 0 );
                buf[sizeof( buf ) - 1] = '\0';
                out += buf;
            }
        }
    }
    return out;
}

void KiwiFxPreview_Step( int msec )
{
    LoadPrefs();
    SyncFromEntities();
    Recount();
    for ( std::map<const void *, std::unique_ptr<instance_t> >::iterator it = s_inst.begin(); it != s_inst.end(); ++it )
        Advance( *it->second, (double)msec );
}

// ── FX browser pictures ────────────────────────────────────────────────────────
// Renders through the Models browser's thumbnail RT (kiwi_entthumb.cpp RenderThumb): an
// orthographic scene in RTT_BeginThumb / RTT_EndThumb, read back through SYSTEMMEM into a
// managed texture (managed textures survive device resets).
namespace
{
    const int   KFXT_RT       = 128;
    const float KFXT_GUARD    = 0.99951171875f;   // camwnd.cpp:236, the build's projection guard
    const float KFXT_BG[4]    = { 0.09f, 0.10f, 0.12f, 1.0f };
    const float KFXT_PITCH    = 12.0f;            // look slightly down at the effect
    const unsigned KFXT_PANE_MS = 33;             // pane frame interval (~30 fps)

    struct thumb_t
    {
        IDirect3DTexture9 *tex = nullptr;
        bool               empty = false;         // rendered, but nothing drew
    };
    std::map<std::string, thumb_t> s_thumbs;      // lower-case fx name
    std::string        s_thumbReq;
    IDirect3DSurface9 *s_readback = nullptr;

    std::unique_ptr<instance_t> s_pane;
    std::string        s_paneFx;
    IDirect3DTexture9 *s_paneTex = nullptr;
    DWORD              s_paneAsked = 0;           // GetTickCount of the last UI request
    DWORD              s_paneLast = 0;
    float              s_paneHalf = 0.0f;         // framing, eased so the view does not jump

    // The fixed browser camera: from -X looking +X, tipped down by KFXT_PITCH.
    void ThumbCamera( const float center[3], float dist, camera_s &cam )
    {
        memset( &cam, 0, sizeof( cam ) );
        const float p = KFXT_PITCH * KFXP_PI / 180.0f;
        cam.vpn[0] = cosf( p ); cam.vpn[1] = 0.0f; cam.vpn[2] = -sinf( p );
        cam.vright[0] = 0.0f; cam.vright[1] = -1.0f; cam.vright[2] = 0.0f;
        Cross( cam.vright, cam.vpn, cam.vup );
        for ( int k = 0; k < 3; ++k )
            cam.origin[k] = center[k] - cam.vpn[k] * dist;
        cam.width = cam.height = KFXT_RT;
    }

    // The effect at the origin, forward = up (a kiwi_fx "angles" of 270 0 0).
    void ThumbInstance( instance_t &in, const std::string &fx, bool repeat )
    {
        in.fx = fx;
        in.repeat = repeat;
        const float ang[3] = { 270.0f, 0.0f, 0.0f }, org[3] = { 0.0f, 0.0f, 0.0f };
        AnglesToFrame( ang, org, in.frame );
        Recount();
        StartInstance( in );
    }

    // Quads of `in` seen from the browser camera, plus their screen-space extent.
    bool CollectThumbQuads( const instance_t &in, const float center[3], float dist, float outCenter[3], float *outHalf )
    {
        camera_s cam;
        ThumbCamera( center, dist, cam );
        s_quads.clear();
        EmitInstance( in, &cam );
        if ( s_quads.empty() )
            return false;
        // Frame where the particles ARE plus a typical sprite size: one huge faint glow
        // (a fire's 300-unit light sprite) must not shrink the flames to a dot.
        float lo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, hi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        std::vector<float> sizes;
        sizes.reserve( s_quads.size() );
        for ( size_t i = 0; i < s_quads.size(); ++i )
        {
            float c[3];
            for ( int k = 0; k < 3; ++k )
            {
                c[k] = ( s_quads[i].xyz[0][k] + s_quads[i].xyz[2][k] ) * 0.5f;
                lo[k] = ( std::min )( lo[k], c[k] );
                hi[k] = ( std::max )( hi[k], c[k] );
            }
            float d2 = 0.0f;
            for ( int k = 0; k < 3; ++k )
                d2 += ( s_quads[i].xyz[0][k] - c[k] ) * ( s_quads[i].xyz[0][k] - c[k] );
            sizes.push_back( sqrtf( d2 ) );
        }
        std::nth_element( sizes.begin(), sizes.begin() + sizes.size() * 3 / 4, sizes.end() );
        const float typical = sizes[sizes.size() * 3 / 4];          // 75th percentile half-diagonal
        float half = 1.0f;                                            // small effects fill the tile too
        for ( int k = 0; k < 3; ++k )
        {
            outCenter[k] = ( lo[k] + hi[k] ) * 0.5f;
            half = ( std::max )( half, ( hi[k] - lo[k] ) * 0.5f + typical );
        }
        *outHalf = half;
        return true;
    }

    // Draws the collected quads into the thumbnail RT framed on center / half, then copies
    // the picture into *tex (created on first use).
    bool RenderQuadsToTexture( const float center[3], float half, IDirect3DTexture9 **tex )
    {
        if ( !dx.device )
            return false;
        camera_s cam;
        ThumbCamera( center, half * 3.0f + 64.0f, cam );
        const float axis[3][3] = {
            {  cam.vpn[0],     cam.vpn[1],     cam.vpn[2]    },
            { -cam.vright[0], -cam.vright[1], -cam.vright[2] },
            {  cam.vup[0],     cam.vup[1],     cam.vup[2]    },
        };
        const float halfExtent = half * 1.08f;
        const float depth = half * 8.0f + 512.0f;
        GfxMatrix proj;
        memset( &proj, 0, sizeof( proj ) );
        proj.m[0][0] = KFXT_GUARD / halfExtent;
        proj.m[1][1] = KFXT_GUARD / halfExtent;
        proj.m[2][2] = KFXT_GUARD / ( 2.0f * depth );
        proj.m[3][2] = KFXT_GUARD * 0.5f;
        proj.m[3][3] = 1.0f;
        if ( !R_Ed_ProjectionWouldBeValid( center, axis, &proj ) )
            return false;
        if ( !RTT_BeginThumb( KFXT_RT, KFXT_RT ) )
            return false;
        const int keepDrawn = s_drawnQuads;      // the camera's diagnostic, not the tile's
        R_BeginFrame();
        R_BeginSharedCmdList();
        R_AddCmdClearScreen( 7, KFXT_BG, 1.0f, 0 );
        R_Ed_SetSceneParms( center, axis, &proj );
        FlushQuads();
        R_EndFrame();
        R_IssueRenderCommands( (uint)-1 );
        RTT_EndThumb();
        s_drawnQuads = keepDrawn;

        IDirect3DSurface9 *rt = RTT_ThumbSurface();
        if ( !rt )
            return false;
        if ( !s_readback && FAILED( dx.device->CreateOffscreenPlainSurface( KFXT_RT, KFXT_RT, D3DFMT_A8R8G8B8,
                                                                          D3DPOOL_SYSTEMMEM, &s_readback, nullptr ) ) )
        {
            s_readback = nullptr;
            return false;
        }
        if ( FAILED( dx.device->GetRenderTargetData( rt, s_readback ) ) )
            return false;
        if ( !*tex && FAILED( dx.device->CreateTexture( KFXT_RT, KFXT_RT, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, tex, nullptr ) ) )
        {
            *tex = nullptr;
            return false;
        }
        D3DLOCKED_RECT src = {}, dst = {};
        if ( FAILED( s_readback->LockRect( &src, nullptr, D3DLOCK_READONLY ) ) )
            return false;
        if ( FAILED( ( *tex )->LockRect( 0, &dst, nullptr, 0 ) ) )
        {
            s_readback->UnlockRect();
            return false;
        }
        for ( int y = 0; y < KFXT_RT; ++y )
        {
            const unsigned *s = (const unsigned *)( (const unsigned char *)src.pBits + (size_t)y * src.Pitch );
            unsigned       *d = (unsigned *)( (unsigned char *)dst.pBits + (size_t)y * dst.Pitch );
            for ( int x = 0; x < KFXT_RT; ++x )
                d[x] = s[x] | 0xFF000000u;       // opaque: ImGui would blend the RT's alpha
        }
        ( *tex )->UnlockRect( 0 );
        s_readback->UnlockRect();
        return true;
    }

    // A tile: one-shots at their busiest moment in the first two seconds, loops at their
    // steady state (StartInstance pre-warms them).
    void RenderTile( const std::string &key )
    {
        thumb_t &t = s_thumbs[key];
        defRt_t *def = Def( key );
        if ( !def || !def->ok )
        {
            t.empty = true;
            return;
        }
        int bestMs = 0;
        if ( !def->looping )
        {
            instance_t probe;
            ThumbInstance( probe, key, false );
            int best = -1;
            for ( int ms = 0; ms <= 2000; ms += 100 )
            {
                const int n = CountParts( probe );
                if ( n > best ) { best = n; bestMs = ms; }
                Advance( probe, 100.0 );
            }
        }
        instance_t in;
        ThumbInstance( in, key, false );
        if ( bestMs > 0 )
            Advance( in, (double)bestMs );
        float center[3] = { 0.0f, 0.0f, 0.0f }, c2[3], half = 1.0f;
        if ( !CollectThumbQuads( in, center, 300.0f, c2, &half ) )
        {
            t.empty = true;
            return;
        }
        // Re-emit from a camera at the framed distance (tails / distance fades read it).
        if ( !CollectThumbQuads( in, c2, half * 3.0f + 64.0f, center, &half ) )
        {
            t.empty = true;
            return;
        }
        t.empty = !RenderQuadsToTexture( center, half, &t.tex );
    }

    void RenderPane()
    {
        if ( s_paneFx.empty() )
            return;
        const DWORD now = ::GetTickCount();
        if ( !s_pane || Key( s_pane->fx.c_str() ) != Key( s_paneFx.c_str() ) )
        {
            s_pane.reset( new instance_t() );
            ThumbInstance( *s_pane, s_paneFx, true );
            s_paneLast = now;
            s_paneHalf = 0.0f;
        }
        if ( now - s_paneLast < KFXT_PANE_MS )
            return;
        double dt = (double)( now - s_paneLast );
        if ( dt > 100.0 ) dt = 100.0;
        s_paneLast = now;
        if ( !s_paused )
            Advance( *s_pane, dt * (double)s_timeScale );
        float center[3] = { 0.0f, 0.0f, 0.0f }, c2[3], half = 1.0f;
        if ( !CollectThumbQuads( *s_pane, center, 300.0f, c2, &half ) )
            return;                              // between bursts: keep the last frame
        // Grow at once, shrink slowly: a burst should not make the view pump.
        s_paneHalf = s_paneHalf <= 0.0f || half > s_paneHalf ? half : s_paneHalf * 0.98f + half * 0.02f;
        c2[0] = c2[1] = 0.0f;                    // keep the effect's own axis centred
        if ( CollectThumbQuads( *s_pane, c2, s_paneHalf * 3.0f + 64.0f, center, &half ) )
        {
            center[0] = center[1] = 0.0f;
            RenderQuadsToTexture( center, ( std::max )( s_paneHalf, half ), &s_paneTex );
        }
    }
}

IDirect3DTexture9 *KiwiFxPreview_Thumb( const char *fxName, bool mayRequest )
{
    const std::string key = Key( fxName );
    if ( key.empty() )
        return nullptr;
    std::map<std::string, thumb_t>::iterator it = s_thumbs.find( key );
    if ( it != s_thumbs.end() && ( it->second.tex || it->second.empty ) )
        return it->second.empty ? nullptr : it->second.tex;
    if ( mayRequest && s_thumbReq.empty() )
        s_thumbReq = key;
    return nullptr;
}

bool KiwiFxPreview_ThumbEmpty( const char *fxName )
{
    std::map<std::string, thumb_t>::const_iterator it = s_thumbs.find( Key( fxName ) );
    return it != s_thumbs.end() && it->second.empty;
}

IDirect3DTexture9 *KiwiFxPreview_Pane( const char *fxName )
{
    const std::string fx = fxName ? fxName : "";
    if ( Key( fx.c_str() ) != Key( s_paneFx.c_str() ) )
    {
        s_paneFx = fx;
        if ( s_paneTex && fx.empty() )
        {
            s_paneTex->Release();
            s_paneTex = nullptr;
        }
    }
    s_paneAsked = ::GetTickCount();
    return fx.empty() ? nullptr : s_paneTex;
}

void KiwiFxPreview_PaneRestart()
{
    s_pane.reset();
}

void KiwiFxPreview_ThumbInvalidate( const char *fxName )
{
    if ( !fxName )
    {
        for ( std::map<std::string, thumb_t>::iterator it = s_thumbs.begin(); it != s_thumbs.end(); ++it )
            if ( it->second.tex )
                it->second.tex->Release();
        s_thumbs.clear();
    }
    else
    {
        std::map<std::string, thumb_t>::iterator it = s_thumbs.find( Key( fxName ) );
        if ( it != s_thumbs.end() )
        {
            if ( it->second.tex )
                it->second.tex->Release();
            s_thumbs.erase( it );
        }
        if ( Key( fxName ) == Key( s_paneFx.c_str() ) )
            s_pane.reset();
    }
}

void KiwiFxPreview_ThumbTick()
{
    if ( !RTT_DeviceHealthy() )
        return;
    // The pane plays while the browser asks for it (it stops when the window closes).
    if ( !s_paneFx.empty() && ::GetTickCount() - s_paneAsked < 500 )
        RenderPane();
    if ( s_thumbReq.empty() )
        return;
    const std::string key = s_thumbReq;
    s_thumbReq.clear();
    RenderTile( key );
}

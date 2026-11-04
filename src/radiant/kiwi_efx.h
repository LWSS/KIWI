#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// KIWI (2026-11-02, user: "FX editor/creator ... live preview ... FX in their own section in
// the outliner"): the .efx effect document shared by the FX editor (kiwi_fxeditor.cpp) and
// the camera preview (kiwi_fxpreview.cpp).
//
// The text format is the stock "iwfx" one: an element is `{ field value; ... }`, fields in
// any order, one visual block per element.  The field set, flag table, curve rules (>= 2
// keys, times 0..1 strictly increasing, values clamped per graph) and defaults follow the
// game's parser, src/EffectsCore/fx_load_obj.cpp (FX_ParseEditorElem / s_elemFields /
// s_allFlagDefs / FX_ParseCurve), so a file this module writes loads in the game, the linker
// (linker_pc64/native_effect.cpp) and the stock effects editor.  The writer always emits
// version 2 with every field, in s_elemFields order, laid out like the shipped files.

#include <string>
#include <vector>

// Element visual types (FxEditorElemDef::elemType values).
enum kefxType_t
{
    KEFX_BILLBOARD = 0,
    KEFX_ORIENTED  = 1,
    KEFX_TAIL      = 2,
    KEFX_TRAIL     = 3,
    KEFX_CLOUD     = 4,
    KEFX_MODEL     = 5,
    KEFX_LIGHT     = 6,
    KEFX_SPOTLIGHT = 7,
    KEFX_SOUND     = 8,
    KEFX_DECAL     = 9,
    KEFX_RUNNER    = 10,
    KEFX_TYPE_COUNT,
    KEFX_NONE      = 11,
};

// editorFlags bits (word 0 of the flag table).
enum
{
    KEFX_E_LOOPING        = 0x1,
    KEFX_E_RANDCOLOR      = 0x2,
    KEFX_E_RANDALPHA      = 0x4,
    KEFX_E_RANDSIZE0      = 0x8,
    KEFX_E_RANDSIZE1      = 0x10,
    KEFX_E_RANDSCALE      = 0x20,
    KEFX_E_RANDROTDELTA   = 0x40,
    KEFX_E_MODCOLORBYALPHA= 0x80,
    KEFX_E_RANDVEL0       = 0x100,
    KEFX_E_RANDVEL1       = 0x200,
    KEFX_E_BACKCOMPATVEL  = 0x400,
    KEFX_E_ABSVEL0        = 0x800,
    KEFX_E_ABSVEL1        = 0x1000,
    KEFX_E_PLAYONTOUCH    = 0x2000,
    KEFX_E_PLAYONDEATH    = 0x4000,
    KEFX_E_PLAYONRUN      = 0x8000,
    KEFX_E_DISABLED       = (int)0x80000000,
};

// flags bits (word 1).
enum
{
    KEFX_F_SPAWNRELATIVE   = 0x2,
    KEFX_F_FRUSTUMCULL     = 0x4,
    KEFX_F_RUNNERRANDROT   = 0x8,
    KEFX_F_OFFSET_MASK     = 0x30,     // none / sphere 0x10 / cylinder 0x20
    KEFX_F_OFFSET_SPHERE   = 0x10,
    KEFX_F_OFFSET_CYLINDER = 0x20,
    KEFX_F_RUN_MASK        = 0xC0,     // world / spawn 0x40 / effect 0x80 / offset 0xC0
    KEFX_F_RUN_SPAWN       = 0x40,
    KEFX_F_RUN_EFFECT      = 0x80,
    KEFX_F_RUN_OFFSET      = 0xC0,
    KEFX_F_NONUNIFORMSCALE = 0x10000000,
};

// atlasBehavior bits (word 2).
enum
{
    KEFX_A_START_MASK    = 0x3,        // fixed / random 1 / indexed 2
    KEFX_A_PLAYOVERLIFE  = 0x4,
    KEFX_A_LOOPONLYN     = 0x8,
};

struct kefxFlagDef_t
{
    const char *name;
    int         word;       // 0 editorFlags, 1 flags, 2 atlasBehavior
    unsigned    mask;
    unsigned    value;
};
// The game's s_allFlagDefs (41 entries), terminated by a null name.
const kefxFlagDef_t *KiwiEfx_FlagDefs();

struct kefxRange_t
{
    float base = 0.0f;      // value = base + random[0,1] * amp
    float amp  = 0.0f;
};

// One curve: keys of (time, value[dims]) flattened; times 0..1 strictly increasing.
struct kefxCurve_t
{
    std::vector<float> keys;
    int Count( int dims ) const { return (int)keys.size() / ( dims + 1 ); }
};

// A graph field: a scale and the min/max curve pair (curve 1 is used by the useRand* flags).
struct kefxGraph_t
{
    float       scale = 1.0f;
    kefxCurve_t curve[2];
};

struct kefxElem_t
{
    std::string name;
    unsigned    editorFlags = 0;
    unsigned    flags       = 0;
    unsigned    atlasBehavior = 0;
    kefxRange_t spawnRange, fadeInRange, fadeOutRange;
    float       spawnFrustumCullRadius = 0.0f;
    int         loopIntervalMsec = 200, loopCount = 0;    // spawnLooping (count 0 = forever)
    kefxRange_t oneShotCount;                              // spawnOneShot (ints)
    kefxRange_t spawnDelayMsec, lifeSpanMsec;              // ints
    kefxRange_t spawnOrg[3], spawnOffsetRadius, spawnOffsetHeight;
    kefxRange_t spawnAngles[3], angleVel[3], initialRot, gravity, elasticity;
    int         atlasIndex = 0, atlasFps = 0, atlasLoopCount = 0;
    int         atlasColIndexBits = 0, atlasRowIndexBits = 0, atlasEntryCount = 0;
    kefxGraph_t vel[2][3];      // velGraph0X..velGraph1Z  (curve values -0.5..0.5)
    kefxGraph_t rot;            // rotGraph                (-0.5..0.5)
    kefxGraph_t size[2];        // sizeGraph0/1            (0..1)
    kefxGraph_t scale;          // scaleGraph              (0..1)
    kefxGraph_t color;          // colorGraph, 3 values per key (0..1)
    kefxGraph_t alpha;          // alphaGraph              (0..1)
    float       lightingFrac = 0.0f;
    float       collOffset[3] = { 0.0f, 0.0f, 0.0f };
    float       collRadius = 0.0f;
    std::string fxOnImpact, fxOnDeath, emission;
    int         sortOrder = 5;
    kefxRange_t emitDist, emitDistVariance;
    int         trailSplitDist = 0;
    float       trailScrollTime = 0.0f;
    int         trailRepeatDist = 0;
    bool        hasTrailDef = false;
    std::vector<float> trailVerts;      // x y texCoord per vertex
    std::vector<int>   trailInds;
    int         type = KEFX_NONE;
    std::vector<std::string> visuals;   // material / model / effect / sound alias names

    bool Looping()  const { return ( editorFlags & KEFX_E_LOOPING ) != 0; }
    bool Disabled() const { return ( editorFlags & KEFX_E_DISABLED ) != 0; }
};

struct kefxDoc_t
{
    std::vector<kefxElem_t> elems;      // at most KEFX_MAX_ELEMS (the game's segment cap)
};

enum { KEFX_MAX_ELEMS = 32, KEFX_MAX_VISUALS = 32, KEFX_MAX_CURVE_KEYS = 128 };

// Curve value range per graph (the game's FX_ParseGraphRange min/max).
void        KiwiEfx_GraphRange( const kefxGraph_t *g, const kefxElem_t &e, float *lo, float *hi );
int         KiwiEfx_GraphDims( const kefxGraph_t *g, const kefxElem_t &e );   // 3 for color

const char *KiwiEfx_TypeName( int type );        // the field keyword ("billboardSprite")
const char *KiwiEfx_TypeLabel( int type );       // UI label ("Billboard sprite")

// Parse .efx text.  False = a syntax error; `err` names the line.  A version-1 file gets
// useBackCompatVel like the game gives it, so re-saving as version 2 keeps its motion.
bool        KiwiEfx_Parse( const char *text, kefxDoc_t &out, std::string &err );
std::string KiwiEfx_Write( const kefxDoc_t &doc );

// raw/fx/<name>.efx next to the editor; name = loadfx path without extension.
std::string KiwiEfx_PathFor( const char *fxName );
bool        KiwiEfx_Load( const char *fxName, kefxDoc_t &out, std::string &err );
// Writes through a temp file + replace; creates folders under raw/fx.
bool        KiwiEfx_Save( const char *fxName, const kefxDoc_t &doc, std::string &err );
// File time of raw/fx/<name>.efx (0 = missing); the preview reloads when it changes.
unsigned long long KiwiEfx_FileStamp( const char *fxName );

// Fresh elements: a looping billboard sprite with the stock defaults the editor shows.
void        KiwiEfx_InitElem( kefxElem_t &e, int type );
// A two-key flat curve (dims values per key).
void        KiwiEfx_FlatCurve( kefxCurve_t &c, int dims, float v );
// Curve sample at t in [0,1]; dim picks the value for colour keys.
float       KiwiEfx_Sample( const kefxCurve_t &c, int dims, float t, int dim = 0 );
// Problems the game would reject or that make an element invisible ("no visuals").
void        KiwiEfx_Validate( const kefxElem_t &e, std::vector<std::string> &out );

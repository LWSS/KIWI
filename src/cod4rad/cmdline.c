/*
 * cmdline.c — Command-line parsing and settings initialization
 */

#include "cod2rad64.h"

/* global settings — initialized by ParseCommandLine_Init, modified by switches */
int g_numThreads;
char g_modelShadows;
char g_extraVerbose2;
char g_disableModelShadows;
char g_unusedFlag1;
char g_unusedFlag2;
char g_relightLoadEnabled;
char g_relightSaveEnabled;
int g_superSampleAlpha;
int g_superSample;
float g_jitter;
int g_traces;
int g_lightmapHeight;
int g_traceFilter;
int g_basisDirCount;
int g_maxBounces;
int g_unusedInt1;
int g_unusedInt2;
int g_unusedInt3;
int g_unusedInt4;
float g_sunDirX        = 0.4418349862f;
float g_sunDirY        = 0.5680699944f;
float g_sunDirZ        = 0.6943129897f;
float g_sunColorR      = 0.7f;
float g_sunColorG      = 0.7f;
float g_sunColorB      = 0.7f;
float g_sunRadiosityR  = 0.7f;
float g_sunRadiosityG  = 0.7f;
float g_sunRadiosityB  = 0.7f;
float g_backfaceLightR = 0.3f;
float g_backfaceLightG = 0.3f;
float g_backfaceLightB = 0.3f;
float g_radiosityScale = 1.0f;
float g_gamma          = 2.2f;
float g_contrastGain   = 0.3f;
char g_radiosityScaleOverridden;
char g_contrastGainOverridden;
char g_verbose;
char g_extraVerbose;
int g_warningLevel;
int g_platform;
char g_platformByte;
char g_aoEnabled;
int g_aoSamples = 32;
float g_aoDist = 128.0f;
char g_adaptiveEnabled;
float g_adaptiveBias = 0.5f;
float g_adaptiveMin = 1.0f;
float g_adaptiveMax = 4.0f;
char g_uvRepackEnabled;
char g_embreeEnabled;

/* default float constants from .rdata */
static const float k_defaultJitter = 0.75f;
static const float k_defaultField_C0 = 0.5680699944f;
static const float k_defaultField_BC = 0.4418349862f;
static const float k_defaultField_C4 = 0.6943129897f;
static const float k_defaultField_C8 = 0.7f;
static const float k_defaultRadiosityScale = 1.0f;
static const float k_defaultGamma = 2.2f;
static const float k_defaultContrastGain = 0.3f;

int             g_defaultPlatform = 0x6E69616D; /* 'main' little-endian */
char            g_defaultPlatformByte = 0;

/* forward decls for handlers used in switch table below */
int PCL_SetVerbose(void);
int PCL_SetExtraVerbose(void);
int PCL_SetWarningLevel(int argc, const char **argv);
int PCL_SetPlatform(int argc, const char **argv);
int PCL_LowQualityPreset(void);
int PCL_HighQualityPreset(void);
int PCL_EnableModelShadows(void);
int PCL_DisableModelShadows(void);
int PCL_SetLightmapSize(int argc, const char **argv);
int PCL_SetJitter(int argc, const char **argv);
int PCL_SetSuperSampleAlpha(int argc, const char **argv);
int PCL_SetSuperSample(int argc, const char **argv);
int PCL_SetMaxBounces(int argc, const char **argv);
int PCL_SetRadiosityScale(int argc, const char **argv);
int PCL_SetTraceFilterLinear(void);
int PCL_SetTraceFilterPoint(void);
int PCL_DisableRelight(void);
int PCL_SetBasisDirCount(int argc, const char **argv);
int PCL_SetBounceFraction(int argc, const char **argv);
int PCL_SetGamma(int argc, const char **argv);
int PCL_SetContrastGain(int argc, const char **argv);
int PCL_SetThreadCount(int argc, const char **argv);
int PCL_SetAO(int argc, const char **argv);
int PCL_SetAOSamples(int argc, const char **argv);
int PCL_SetAODist(int argc, const char **argv);
int PCL_SetAdaptive(int argc, const char **argv);
int PCL_SetAdaptiveBias(int argc, const char **argv);
int PCL_SetAdaptiveMin(int argc, const char **argv);
int PCL_SetAdaptiveMax(int argc, const char **argv);
int PCL_SetUVRepack(int argc, const char **argv);
int PCL_EnableEmbree(void);
int PCL_DisplaySettings(void);

CmdlineSwitch_t g_cmdlineSwitches[NUM_CMDLINE_SWITCHES] = {
    { "-Verbose",        "Turns on verbose prints",                                  (CmdlineHandler_f)PCL_SetVerbose },
    { "-Quiet",          "Turns off progress counters (can be used with -Verbose)", (CmdlineHandler_f)PCL_SetExtraVerbose },
    { "-Warn",           "Sets the warning level",                                   PCL_SetWarningLevel },
    { "-Platform",       "Target platform (pc, etc)",                                PCL_SetPlatform },
    { "-Fast",           "Use fast presets for several options",                    (CmdlineHandler_f)PCL_LowQualityPreset },
    { "-Extra",          "Use high-quality presets for several options",            (CmdlineHandler_f)PCL_HighQualityPreset },
    { "-ModelShadow",    "Allows model surfaces to cast shadows",                   (CmdlineHandler_f)PCL_EnableModelShadows },
    { "-NoModelShadow",  "Prevents model surfaces from casting shadows",            (CmdlineHandler_f)PCL_DisableModelShadows },
    { "-Traces",         "Number of traces to do from each sample point",           PCL_SetLightmapSize },
    { "-Jitter",         "Breaks up aliasing from trace pattern (0 none, 1 max)",   PCL_SetJitter },
    { "-SuperSampleAlpha", "Does N lookups to antialias each alpha mask test",      PCL_SetSuperSampleAlpha },
    { "-SuperSample",    "Turns each sample into NxN samples instead",              PCL_SetSuperSample },
    { "-MaxBounces",     "Stops radiosity after N bounces if it hasn't settled",    PCL_SetMaxBounces },
    { "-RadiosityScale", "Scales intensity of all bounced light; 1 is no change",   PCL_SetRadiosityScale },
    { "-TraceFilterLinear", "Linearly filter lightmap pixels hit by radiosity traces", (CmdlineHandler_f)PCL_SetTraceFilterLinear },
    { "-TraceFilterPoint", "Point sample lightmap pixels hit by  radiosity traces", (CmdlineHandler_f)PCL_SetTraceFilterPoint },
    { "-Gamma",          "Gamma value assumed to be implicitly stored in textures", PCL_SetGamma },
    { "-ContrastGain",   "Increase lighting contrast (0 no change, 1 max)",         PCL_SetContrastGain },
    { "-NoRelight",      "Disable optimization of using results from last compile", (CmdlineHandler_f)PCL_DisableRelight },
    { "-BasisDirCount",  "Sample directions used to approximate lightmap pixel",    PCL_SetBasisDirCount },
    { "-Threads",        "Allows using more or fewer threads than processors",     PCL_SetThreadCount },
    { "-DumpOptions",    "Displays current settings of most parameters",           (CmdlineHandler_f)PCL_DisplaySettings },

    /* Non-retail extensions remain accepted but are intentionally omitted
     * from the native usage block. */
    { "-BounceFraction", "Compatibility alias for -RadiosityScale",                 PCL_SetBounceFraction },
    { "-AO",             "Enable ambient occlusion (0=off, 1=on)",                 PCL_SetAO },
    { "-AOSamples",      "Number of AO hemisphere rays per texel (default 32)",    PCL_SetAOSamples },
    { "-AODist",         "Max AO ray distance in world units (default 128)",       PCL_SetAODist },
    { "-Adaptive",       "Enable adaptive lightmap resolution (0=off, 1=on)",      PCL_SetAdaptive },
    { "-AdaptiveBias",   "Aggressiveness 0.0=conservative, 1.0=aggressive",        PCL_SetAdaptiveBias },
    { "-AdaptiveMin",    "Min samplescale for uniform surfaces (default 1.0)",      PCL_SetAdaptiveMin },
    { "-AdaptiveMax",    "Max samplescale for shadow-edge surfaces (default 4.0)",  PCL_SetAdaptiveMax },
    { "-UV",             "Enable tight UV repack (Frostbite-style, 0=off, 1=on)",   PCL_SetUVRepack },
    { "-Embree",         "Use the optional Embree ray tracer",                     (CmdlineHandler_f)PCL_EnableEmbree },
};

/*
================
ParseCommandLine_Init

Set all radiosity settings to defaults.
================
*/
void ParseCommandLine_Init(void)
{
    SYSTEM_INFO sysInfo;
    int numProcs;

    GetSystemInfo(&sysInfo);
    numProcs = (int)sysInfo.dwNumberOfProcessors;

    if (numProcs < 1)
        numProcs = 1;
    else if (numProcs > 4)
        numProcs = 4;

    g_warningLevel = 4;
    g_numThreads = numProcs;

    g_modelShadows = 1;
    g_extraVerbose2 = 0;
    g_disableModelShadows = 0;
    g_unusedFlag1 = 1;
    g_unusedFlag2 = 1;
    g_relightLoadEnabled = 1;
    g_relightSaveEnabled = 1;

    g_jitter = k_defaultJitter;
    g_sunDirY = k_defaultField_C0;
    g_sunDirX = k_defaultField_BC;
    g_sunDirZ = k_defaultField_C4;
    g_sunColorR = k_defaultField_C8;
    g_sunColorG = k_defaultField_C8;
    g_sunColorB = k_defaultField_C8;
    g_sunRadiosityR = k_defaultField_C8;
    g_sunRadiosityG = k_defaultField_C8;
    g_sunRadiosityB = k_defaultField_C8;
    g_backfaceLightR = k_defaultContrastGain;
    g_backfaceLightG = k_defaultContrastGain;
    g_backfaceLightB = k_defaultContrastGain;
    g_radiosityScale = k_defaultRadiosityScale;
    g_gamma = k_defaultGamma;
    g_contrastGain = k_defaultContrastGain;

    g_aoEnabled = 0;
    g_aoSamples = 32;
    g_aoDist = 128.0f;
    g_adaptiveEnabled = 0;
    g_adaptiveBias = 0.5f;
    g_adaptiveMin = 4.0f;
    g_adaptiveMax = 32.0f;
    g_uvRepackEnabled = 0;
    g_embreeEnabled = 0;

    g_superSampleAlpha = 4;
    g_superSample = 2;
    g_traces = 0x20;
    g_lightmapHeight = 0x40;
    g_traceFilter = 1;
    g_basisDirCount = 0x20;
    g_maxBounces = 0x20;
    g_unusedInt1 = 0x20;
    g_unusedInt2 = 0x18;
    g_unusedInt3 = 0x10;
    g_unusedInt4 = 0x18;

    g_radiosityScaleOverridden = 0;
    g_contrastGainOverridden = 0;
    g_verbose = 0;
    g_extraVerbose = 0;

    g_platform = g_defaultPlatform;
    g_platformByte = g_defaultPlatformByte;
}

static char s_assertDisable_ParseInt_argc;
static char s_assertDisable_ParseInt_range;

/*
================
PCL_ParseInt

Parse integer arg from argv[1], clamp to [min, max].
================
*/
int PCL_ParseInt(int argc, const char **argv, int *output, int min, int max)
{
    int value;

    Assert(argc >= 1, s_assertDisable_ParseInt_argc);
    Assert(min < max, s_assertDisable_ParseInt_range);

    if (argc < 2)
    {
        ErrorMsg("%s: missing argument\n", argv[0]);
    }

    value = atoi(argv[1]);
    *output = value;

    if (value < min)
    {
        WarningMsg(1, "%s: clamping to %i\n", argv[0], min);
        *output = min;
    }
    else if (value > max)
    {
        WarningMsg(1, "%s: clamping to %i\n", argv[0], max);
        *output = max;
    }

    return 2;
}

static char s_assertDisable_ParseFloat_argc;
static char s_assertDisable_ParseFloat_range;

/*
================
PCL_ParseFloat

Parse float arg from argv[1], clamp to [min, max].
================
*/
int PCL_ParseFloat(int argc, const char **argv, float *output, float min, float max)
{
    float value;

    Assert(argc >= 1, s_assertDisable_ParseFloat_argc);
    Assert(min < max, s_assertDisable_ParseFloat_range);

    if (argc < 2)
    {
        ErrorMsg("%s: missing argument\n", argv[0]);
    }

    value = (float)atof(argv[1]);
    *output = value;

    if (value < min)
    {
        WarningMsg(1, "%s: clamping to %g\n", argv[0], (double)min);
        *output = min;
    }
    else if (value > max)
    {
        WarningMsg(1, "%s: clamping to %g\n", argv[0], (double)max);
        *output = max;
    }

    return 2;
}

/*
================
PCL_SetVerbose

Enable verbose output.
================
*/
int PCL_SetVerbose(void)
{
    g_verbose = 1;
    return 1;
}

/*
================
PCL_SetExtraVerbose

Enable extra verbose output.
================
*/
int PCL_SetExtraVerbose(void)
{
    g_extraVerbose = 1;
    return 1;
}

/*
================
PCL_SetWarningLevel

Set warning level, clamp [0, 4].
================
*/
int PCL_SetWarningLevel(int argc, const char **argv)
{
    return PCL_ParseInt(argc, argv, &g_warningLevel, 0, 4);
}

static char s_assertDisable_SetPlatform;

/*
================
PCL_SetPlatform

Parse target platform name from argv[1].
================
*/
int PCL_SetPlatform(int argc, const char **argv)
{
    SetTargetPlatformByName(argv[1]);

    Assert(g_targetPlatform != 0, s_assertDisable_SetPlatform);

    return 2;
}

/*
================
PCL_LowQualityPreset

Fast: shadows off, ss=1, traces=16, lmHeight=32.
================
*/
int PCL_LowQualityPreset(void)
{
    g_modelShadows = 0;
    g_extraVerbose2 = 0;
    g_superSample = 1;
    g_traces = 0x10;
    g_lightmapHeight = 0x20;
    g_basisDirCount = 0x10;
    return 1;
}

/*
================
PCL_HighQualityPreset

Extra: shadows on, ss=3, traces=64, lmHeight=128.
================
*/
int PCL_HighQualityPreset(void)
{
    g_modelShadows = 1;
    g_extraVerbose2 = 1;
    g_superSample = 4;
    g_traces = 0x30;
    g_lightmapHeight = 0x60;
    g_basisDirCount = 0x40;
    return 1;
}

/*
================
PCL_EnableModelShadows
================
*/
int PCL_EnableModelShadows(void)
{
    g_modelShadows = 1;
    return 1;
}

/*
================
PCL_DisableModelShadows
================
*/
int PCL_DisableModelShadows(void)
{
    g_modelShadows = 0;
    return 1;
}

/*
================
PCL_SetLightmapSize

Parse trace count, clamp [16, 512], set direction count = traces * 2.
================
*/
int PCL_SetLightmapSize(int argc, const char **argv)
{
    int result;

    result = PCL_ParseInt(argc, argv, &g_traces, 0x10, 0x200);
    g_lightmapHeight = g_traces * 2;

    return result;
}

/*
================
PCL_SetJitter

Parse jitter, clamp [0, 1].
================
*/
int PCL_SetJitter(int argc, const char **argv)
{
    return PCL_ParseFloat(argc, argv, &g_jitter, 0.0f, 1.0f);
}

int PCL_SetSuperSampleAlpha(int argc, const char **argv)
{
    return PCL_ParseInt(argc, argv, &g_superSampleAlpha, 1, 0x1F);
}

/*
================
PCL_SetSuperSample

Parse supersample, clamp [1, 8].
================
*/
int PCL_SetSuperSample(int argc, const char **argv)
{
    return PCL_ParseInt(argc, argv, &g_superSample, 1, 8);
}

int PCL_SetMaxBounces(int argc, const char **argv)
{
    return PCL_ParseInt(argc, argv, &g_maxBounces, 1, 0x100);
}

int PCL_SetRadiosityScale(int argc, const char **argv)
{
    g_radiosityScaleOverridden = 1;
    return PCL_ParseFloat(argc, argv, &g_radiosityScale, 0.0f, 10.0f);
}

int PCL_SetTraceFilterLinear(void)
{
    g_traceFilter = 4;
    return 1;
}

int PCL_SetTraceFilterPoint(void)
{
    g_traceFilter = 1;
    return 1;
}

int PCL_DisableRelight(void)
{
    g_relightLoadEnabled = 0;
    g_relightSaveEnabled = 0;
    return 1;
}

int PCL_SetBasisDirCount(int argc, const char **argv)
{
    return PCL_ParseInt(argc, argv, &g_basisDirCount, 0x10, 0x100);
}

/*
================
PCL_SetBounceFraction

Parse bounce fraction, clamp [0, 0.9], set override flag.
================
*/
int PCL_SetBounceFraction(int argc, const char **argv)
{
    return PCL_SetRadiosityScale(argc, argv);
}

/*
================
PCL_SetGamma

Parse gamma, clamp [0.25, 4.0].
================
*/
int PCL_SetGamma(int argc, const char **argv)
{
    return PCL_ParseFloat(argc, argv, &g_gamma, 0.25f, 4.0f);
}

/*
================
PCL_SetContrastGain

Parse contrast gain, clamp [0, 1.0], set override flag.
================
*/
int PCL_SetContrastGain(int argc, const char **argv)
{
    g_contrastGainOverridden = 1;
    return PCL_ParseFloat(argc, argv, &g_contrastGain, 0.0f, 1.0f);
}

/*
================
PCL_SetThreadCount

Parse thread count from command line. Minimum 1, no upper cap.
================
*/
int PCL_SetThreadCount(int argc, const char **argv)
{
    return PCL_ParseInt(argc, argv, &g_numThreads, 1, 4);
}

int PCL_SetAO(int argc, const char **argv)
{
    int val;
    int result = PCL_ParseInt(argc, argv, &val, 0, 1);
    g_aoEnabled = (char)val;
    return result;
}

int PCL_SetAOSamples(int argc, const char **argv)
{
    return PCL_ParseInt(argc, argv, &g_aoSamples, 4, 256);
}

int PCL_SetAODist(int argc, const char **argv)
{
    return PCL_ParseFloat(argc, argv, &g_aoDist, 1.0f, 4096.0f);
}

int PCL_SetAdaptive(int argc, const char **argv)
{
    int val;
    int result = PCL_ParseInt(argc, argv, &val, 0, 1);
    g_adaptiveEnabled = (char)val;
    return result;
}

int PCL_SetAdaptiveBias(int argc, const char **argv)
{
    return PCL_ParseFloat(argc, argv, &g_adaptiveBias, 0.0f, 1.0f);
}

int PCL_SetAdaptiveMin(int argc, const char **argv)
{
    return PCL_ParseFloat(argc, argv, &g_adaptiveMin, 0.25f, 8.0f);
}

int PCL_SetAdaptiveMax(int argc, const char **argv)
{
    return PCL_ParseFloat(argc, argv, &g_adaptiveMax, 0.5f, 16.0f);
}

int PCL_SetUVRepack(int argc, const char **argv)
{
    int val;
    int result = PCL_ParseInt(argc, argv, &val, 0, 1);
    g_uvRepackEnabled = (char)val;
    return result;
}

int PCL_EnableEmbree(void)
{
    g_embreeEnabled = 1;
    return 1;
}

/*
================
PCL_DisplaySettings

Print current radiosity settings.
================
*/
int PCL_DisplaySettings(void)
{
    Com_Printf("%-30s %i\n", "number of threads:", g_numThreads);
    Com_Printf("%-30s %g\n", "radiosity scale:", (double)g_radiosityScale);
    Com_Printf("%-30s %g\n", "contrast gain:", (double)g_contrastGain);
    Com_Printf("%-30s %g\n", "gamma:", (double)g_gamma);
    Com_Printf("%-30s %i\n", "max bounces:", g_maxBounces);
    Com_Printf("%-30s %i\n", "traces:", g_traces);
    Com_Printf("%-30s %g\n", "jitter:", (double)g_jitter);
    Com_Printf("%-30s %i\n", "alpha super sampling:", g_superSampleAlpha);
    Com_Printf("%-30s %i\n", "lightmap super sampling:", g_superSample);
    Com_Printf("%-30s %s\n", "linear filter radiosity traces:", g_traceFilter == 4 ? "enabled" : "disabled");
    Com_Printf("%-30s %s\n", "model shadows:", g_modelShadows ? "enabled" : "disabled");
    Com_Printf("%-30s %i\n", "basis direction count:", g_basisDirCount);
    Com_Printf("%-30s %s\n", "verbose messages:", g_verbose ? "enabled" : "disabled");

    return 1;
}

static int PCL_PrintUsage(void)
{
    int i;

    Com_Printf("USAGE: cod2rad [args] mapname, where args is 0 or more of the following.\n");
    Com_Printf("Options ignore capitalization; it is only present in the list for clarity.\n");
    for (i = 0; i < NUM_NATIVE_CMDLINE_SWITCHES; i++)
    {
        Com_Printf("%-20s %s\n", g_cmdlineSwitches[i].name,
                   g_cmdlineSwitches[i].description);
    }
    return 1;
}

/*
================
PCL_ParseArgs

Match argv entries against switch table, dispatch handlers.
================
*/
int PCL_ParseArgs(int startIdx, const char **argv, int endIdx)
{
    int i;
    CmdlineSwitch_t *entry;
    int consumed;

    if (startIdx <= endIdx)
        goto check_done;

    do
    {
        /* search switch table for matching argument */
        i = 0;
        entry = g_cmdlineSwitches;

        while (i < NUM_CMDLINE_SWITCHES)
        {
            if (I_stricmp(argv[0], entry->name) == 0)
            {
                consumed = entry->handler(startIdx, argv);
                if (consumed <= 0)
                {
                    PCL_PrintUsage();
                    return 0;
                }

                startIdx -= consumed;
                argv += consumed;
                break;
            }

            i++;
            entry++;
        }

        if (i == NUM_CMDLINE_SWITCHES)
        {
            PCL_PrintUsage();
            Com_Printf("\n");
            ErrorMsg("Unknown argument '%s'\n", argv[0]);
        }

    check_done:
        ;
    } while (startIdx > endIdx);

    if (startIdx == endIdx)
        return 1;

    PCL_PrintUsage();
    return 0;
}

extern void __security_check_cookie(unsigned __int64);

static char s_assertDisable_ParseCmdLine_argc;
static char s_assertDisable_ParseCmdLine_plat;

char g_mapName[MAX_OS_PATH_SHORT];
char g_bspPath[MAX_OS_PATH_SHORT];
char g_bspExtension[MAX_FILE_EXT];

/*
================
ParseCommandLine

Main entry point for command-line parsing.
================
*/
int ParseCommandLine(int argc, const char **argv)
{
    char progPath[0x418];
    char *slashFwd, *slashBack, *lastSep;
    char *dot;
    char *bspResult;
    int i;

    ParseCommandLine_Init();

    /* copy argv[0] to local buffer and strip filename to get directory */
    {
        const char *src = argv[0];
        char *dst = progPath;
        while ((*dst++ = *src++) != '\0')
            ;
    }

    slashFwd = strrchr(progPath, '/');
    slashBack = strrchr(progPath, '\\');

    if (slashFwd > slashBack)
        lastSep = strrchr(progPath, '/');
    else
        lastSep = strrchr(progPath, '\\');

    if (lastSep)
        *lastSep = '\0';

    Assert(argc >= 1, s_assertDisable_ParseCmdLine_argc);

    argc--;
    if (argc == 0)
    {
        PCL_PrintUsage();
        return 0;
    }

    SetBspFileExtensions("d3d");

    /* copy last argument (map name) to g_gridLogBasePath and g_mapName */
    {
        extern char g_gridLogBasePath[];
        const char *src = argv[argc];
        char *dst;

        /* copy to g_gridLogBasePath */
        dst = g_gridLogBasePath;
        while ((*dst++ = *src++) != '\0')
            ;

        /* strip extension from g_gridLogBasePath */
        {
            char *slFwd = strrchr(g_gridLogBasePath, '/');
            char *slBack = strrchr(g_gridLogBasePath, '\\');
            char *lastSl = (slFwd > slBack) ? slFwd : slBack;
            char *d = strrchr(g_gridLogBasePath, '.');
            if (d && (lastSl == NULL || d > lastSl))
                *d = '\0';
        }

        /* copy to g_mapName */
        src = argv[argc];
        dst = g_mapName;
        while ((*dst++ = *src++) != '\0')
            ;
    }

    /* strip path from map name */
    slashFwd = strrchr(g_mapName, '/');
    slashBack = strrchr(g_mapName, '\\');

    if (slashFwd > slashBack)
        lastSep = strrchr(g_mapName, '/');
    else
        lastSep = strrchr(g_mapName, '\\');

    /* strip extension */
    dot = strrchr(g_mapName, '.');
    if (dot != 0)
    {
        if (lastSep == 0 || dot > lastSep)
            *dot = '\0';
    }

    /* append BSP extension to map name */
    bspResult = GetBspFileExtension();
    {
        int len = 0;
        while (g_mapName[len] != '\0')
            len++;
        {
            const char *src = bspResult;
            char *dst = &g_mapName[len];
            while ((*dst++ = *src++) != '\0')
                ;
        }
    }

    if (!PCL_ParseArgs(argc, argv + 1, 1))
        return 0;

    if (!ValidatePlatformSet())
        return 0;

    if (g_mapName[0] == '-' || g_mapName[0] == '?')
        return 0;

    Assert(g_targetPlatform != 0, s_assertDisable_ParseCmdLine_plat);

    {
        extern char g_basePath[1024];
        CreatePath(g_mapName);
        InitFileSystem(g_basePath, "", "");
    }

    return 1;
}


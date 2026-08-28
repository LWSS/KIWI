/*
 * mapio.c — Map file reading, entity processing, and BSP writing.
 *
 * All functions verified against cod2rad64 LST line by line.
 * Source: mapio.cpp (from LST source tags and assert strings: .\\mapio.cpp)
 */

#include "cod2rad64.h"

extern double strtof_wrap(const char *str); /* sub_43C920: returns double (cvtsd2ss at call sites) */

/* BSP_Finalize, WriteBSPFile — in cod2rad64.h */

/*
 * BspFileHeader — BSP file header.
 * +0x10: total data size (int).
 */
typedef struct BspFileHeader
{
    unsigned char pad00[0x10];  /* +0x00: magic, version, etc */
    int dataSize;               /* +0x10: total BSP data size */
} BspFileHeader;

BspFileHeader *g_bspData;

extern int num_entities; /* defined in bspfile.c */

/*
 * ValueForKey — find a value string for a given key on an entity.
 * Address: 0x4153A0 | Size: 236 bytes
 *
 * rcx=entity, rdx=key
 * Returns value string pointer, or NULL if not found.
 */
const char *ValueForKey(Entity_t *entity, const char *key)
{
    KeyValuePair_t *kv;

    /* assert: entity != NULL (line 0x2D) */
    Assert("entity", ".\\mapio.cpp", 0x2D, 0, 1);

    /* assert: key != NULL (line 0x2E) */
    Assert("key", ".\\mapio.cpp", 0x2E, 0, 1);

    /* walk key-value list */
    kv = entity->keyValues;
    while (kv)
    {
        if (I_stricmp(kv->key, key) == 0)
            return kv->value;
        kv = kv->next;
    }

    return NULL;
}

/*
 * ParseVectorString — look up a key on an entity and parse its value as a float[3].
 * Address: 0x415490 | Size: 259 bytes
 *
 * rcx=entity, rdx=key, r8=outVector (float[3])
 * Returns 1 (al) if parsed successfully, 0 if not found or parse failed.
 *
 * Tries 4 sscanf format strings: "%g %g %g", "%g, %g, %g",
 * "( %g %g %g )", "( %g, %g, %g )".
 */
extern int sscanf_wrap(const char *str, const char *fmt, ...);

int ParseVectorString(Entity_t *entity, const char *key, float *outVector)
{
    const char *value;

    value = ValueForKey(entity, key);
    if (!value)
        return 0;

    /* try multiple vector formats */
    if (sscanf_wrap(value, "%g %g %g", &outVector[0], &outVector[1], &outVector[2]) == 3)
        return 1;
    if (sscanf_wrap(value, "%g, %g, %g", &outVector[0], &outVector[1], &outVector[2]) == 3)
        return 1;
    if (sscanf_wrap(value, "( %g %g %g )", &outVector[0], &outVector[1], &outVector[2]) == 3)
        return 1;
    if (sscanf_wrap(value, "( %g, %g, %g )", &outVector[0], &outVector[1], &outVector[2]) == 3)
        return 1;

    WarningMsg(2, "key '%s' has value '%s' which is not a valid vector\n", key, value);
    return 0;
}

/*
 * FindEntityWithKeyValue — find an entity with a specific key=value pair.
 * Address: 0x4155A0 | Size: 311 bytes
 *
 * rcx=key, rdx=value
 * Returns Entity_t* or NULL.
 *
 * Iterates all entities, for each walks its key-value list looking for
 * a matching key, then checks if the value also matches.
 */
Entity_t *FindEntityWithKeyValue(const char *key, const char *value)
{
    int i;

    for (i = 0; i < num_entities; i++)
    {
        Entity_t *ent = &g_entities[i];
        KeyValuePair_t *kv;

        /* assert: entity != NULL (line 0x2D) */
        Assert("entity", ".\\mapio.cpp", 0x2D, 0, 1);

        /* assert: key != NULL (line 0x2E) */
        Assert("key", ".\\mapio.cpp", 0x2E, 0, 1);

        /* walk key-value list */
        kv = ent->keyValues;
        while (kv)
        {
            if (I_stricmp(kv->key, key) == 0)
            {
                /* key matches — check value */
                if (kv->value && I_stricmp(value, kv->value) == 0)
                    return ent;
                break;
            }
            kv = kv->next;
        }
    }

    return NULL;
}

/*
 * ProcessBrushModelTriangles — process triangles for a brush model entity.
 * Address: 0x416540 | Size: 283 bytes
 *
 * rcx=entity, rdx=modelValue (string starting with '*')
 * Parses model index, validates, gets transform, iterates triangles.
 */
extern int atoi_wrap(const char *str);

/* dword_1268FC80 = bspTriangles[] (bspfile.c), same static array */
extern void AddTrianglesForSurface(void *tri, int modelIndex, float *transform); /* geometry_40BBE0 */

void ProcessBrushModelTriangles(Entity_t *entity, const char *modelValue)
{
    int modelIndex;
    float transform[12]; /* 48 bytes: origin + rotation matrix */
    int i;

    /* parse model index (skip '*' prefix) */
    modelIndex = atoi_wrap(modelValue + 1);

    if (modelIndex <= 0)
    {
        /* compute entity index from pointer difference */
        int entityIdx = (int)((long long)entity - (long long)g_entities) / 0x38;
        WarningMsg(1, "Entity_t %i has bad brush model %s\n", entityIdx, modelValue);
        return;
    }

    if (modelIndex >= numBSPModels)
    {
        WarningMsg(1, "ignoring bad model index %i\n", modelIndex);
        return;
    }

    /* get entity transform (origin + rotation) */
    GetEntityOriginAndAngles(entity, transform);

    /* iterate triangles for this brush model (bspModels[] at dword_E020360) */
    {
        BspModel_t *model = &bspModels[modelIndex];

        for (i = 0; i < model->numTriSoups; i++)
        {
            AddTrianglesForSurface(&bspTriangles[model->firstTriSoup + i], modelIndex, transform);
        }
    }
}

/*
 * ProcessEntity — process the worldspawn entity.
 * Address: 0x415990 | Size: 2985 bytes
 *
 * rcx=entity
 *
 * Reads lighting configuration from entity keys (radiosityScale, ambient,
 * sun direction, sun light, etc.), then iterates all BSP surfaces and
 * processes their geometry triangles.
 *
 * This is the largest function in the binary at 2985 bytes.
 * The structure is: read config → iterate surfaces → process tris.
 */
extern float g_ambientColor[3];     /* dword_4808E0..E8 */
/* g_sunDirX/Y/Z and g_backfaceLightR/G/B are in cod2rad64.h */
extern int g_bspSurfCount;          /* runtime global */
extern void *g_bspSurfs;            /* runtime global: BSP surface array */
extern void ProcessSurfaceGeometry(Entity_t *entity, int surfIndex, void *material); /* various geometry calls */
extern void SetupSunLight(float *sunDir, float *sunColor, float intensity); /* various setup calls */

extern char g_radiosityScaleOverridden;
extern char g_contrastGainOverridden;
extern int g_bspTriSoupCount;          /* runtime */
extern void *g_bspTriSoups;            /* runtime: BSP triSoup array */
extern void ProcessWorldSurface(int surfIdx, float *origin, float *axis, void *material); /* various geometry calls */

/*
 * ProcessEntity — process worldspawn entity lighting configuration and surfaces.
 * Address: 0x415990 | Size: 2985 bytes
 *
 * rcx=entity
 *
 * Reads: radiosityScale, contrastGain, ambient, diffuseFraction, sunLight, sunDirection,
 * sunColor, and other lighting parameters from entity key-value pairs.
 * Then iterates all BSP triSoups and processes their geometry.
 *
 * For each parameter: check command-line override, else parse from entity,
 * clamp to valid range, print configuration message.
 */
void ProcessEntity(Entity_t *entity)
{
    const char *value;
    float parsed;

    /* radiosityScale */
    if (g_radiosityScaleOverridden)
    {
        Com_Printf("\nusing command line %s %g\n\n", "radiosityScale", (double)g_radiosityScale);
    }
    else
    {
        /* assert: entity != NULL (line 0x2D) */
        Assert("entity", ".\\mapio.cpp", 0x2D, 0, 1);

        value = ValueForKey(entity, "radiosityScale");
        if (!value)
        {
            Com_Printf("\nusing default %s %g\n\n", "radiosityScale", (double)g_radiosityScale);
        }
        else
        {
            parsed = (float)strtof_wrap(value);
            g_radiosityScale = parsed;
            if (parsed < 0.0f)
            {
                Com_Printf("map %s clamped from %g to %g\n\n", "radiosityScale", (double)parsed, 0.0);
                g_radiosityScale = 0.0f;
            }
            else if (parsed > 10.0f)
            {
                Com_Printf("map %s clamped from %g to %g\n\n", "radiosityScale", (double)parsed, 10.0);
                g_radiosityScale = 10.0f;
            }
            else
            {
                Com_Printf("\nusing map radiosityScale %g\n\n", (double)parsed);
            }
        }
    }

    /* contrastGain — same pattern */
    if (g_contrastGainOverridden)
    {
        Com_Printf("\nusing command line %s %g\n\n", "contrastGain", (double)g_contrastGain);
    }
    else
    {
        Assert("entity", ".\\mapio.cpp", 0x2D, 0, 1);

        value = ValueForKey(entity, "contrastGain");
        if (!value)
        {
            Com_Printf("\nusing default %s %g\n\n", "contrastGain", (double)g_contrastGain);
        }
        else
        {
            parsed = (float)strtof_wrap(value);
            g_contrastGain = parsed;
            if (parsed < 0.0f)
            {
                Com_Printf("map %s clamped from %g to %g\n\n", "contrastGain", (double)parsed, 0.0);
                g_contrastGain = 0.0f;
            }
            else if (parsed > 1.0f)
            {
                Com_Printf("map %s clamped from %g to %g\n\n", "contrastGain", (double)parsed, 1.0);
                g_contrastGain = 1.0f;
            }
            else
            {
                Com_Printf("\nusing map contrastGain %g\n\n", (double)g_contrastGain);
            }
        }
    }

    /* Native worldspawn lighting setup recovered from cod4rad.exe. */

    /* sunlight (float intensity) */
    Assert("entity", ".\\mapio.cpp", 0x2D, 0, 1);
    float sunIntensity = 0.0f;
    value = ValueForKey(entity, "sunlight");
    if (value)
        sunIntensity = (float)strtof_wrap(value);

    /* Retail uses a separate directional intensity for bounce seeding and
     * falls back to sunlight when the key is absent or non-positive. */
    float sunRadiosity = sunIntensity;
    value = ValueForKey(entity, "sunradiosity");
    if (value)
    {
        sunRadiosity = (float)strtof_wrap(value);
        if (sunRadiosity <= 0.0f)
            sunRadiosity = sunIntensity;
    }

    /* suncolor (RGB, normalized by max component) */
    float sunColor[3] = {0.0f, 0.0f, 0.0f};
    {
        const char *sc = ValueForKey(entity, "suncolor");
        if (sc)
        {
            if (sscanf_wrap(sc, "%g %g %g", &sunColor[0], &sunColor[1], &sunColor[2]) != 3 &&
                sscanf_wrap(sc, "%g, %g, %g", &sunColor[0], &sunColor[1], &sunColor[2]) != 3 &&
                sscanf_wrap(sc, "( %g %g %g )", &sunColor[0], &sunColor[1], &sunColor[2]) != 3 &&
                sscanf_wrap(sc, "( %g, %g, %g )", &sunColor[0], &sunColor[1], &sunColor[2]) != 3)
                WarningMsg(2, "key '%s' has value '%s' which is not a valid vector\n", "suncolor", sc);
            else
            {
                float mc = sunColor[0];
                if (sunColor[1] > mc) mc = sunColor[1];
                if (sunColor[2] > mc) mc = sunColor[2];
                if (mc != 0.0f) { float inv = 1.0f / mc; sunColor[0] *= inv; sunColor[1] *= inv; sunColor[2] *= inv; }
            }
        }
    }

    /* diffusefraction */
    float diffuseFrac = 0.0f;
    value = ValueForKey(entity, "diffusefraction");
    if (value)
        diffuseFrac = (float)strtof_wrap(value);

    /* sundiffusecolor (RGB, normalized by max component) */
    float sunDiffColor[3] = {0.0f, 0.0f, 0.0f};
    {
        const char *sd = ValueForKey(entity, "sundiffusecolor");
        if (sd)
        {
            if (sscanf_wrap(sd, "%g %g %g", &sunDiffColor[0], &sunDiffColor[1], &sunDiffColor[2]) != 3 &&
                sscanf_wrap(sd, "%g, %g, %g", &sunDiffColor[0], &sunDiffColor[1], &sunDiffColor[2]) != 3 &&
                sscanf_wrap(sd, "( %g %g %g )", &sunDiffColor[0], &sunDiffColor[1], &sunDiffColor[2]) != 3 &&
                sscanf_wrap(sd, "( %g, %g, %g )", &sunDiffColor[0], &sunDiffColor[1], &sunDiffColor[2]) != 3)
                WarningMsg(2, "key '%s' has value '%s' which is not a valid vector\n", "sundiffusecolor", sd);
            else
            {
                float mc = sunDiffColor[0];
                if (sunDiffColor[1] > mc) mc = sunDiffColor[1];
                if (sunDiffColor[2] > mc) mc = sunDiffColor[2];
                if (mc != 0.0f) { float inv = 1.0f / mc; sunDiffColor[0] *= inv; sunDiffColor[1] *= inv; sunDiffColor[2] *= inv; }
            }
        }
    }

    /* ambient (float) */
    float ambientVal = 0.0f;
    value = ValueForKey(entity, "ambient");
    if (value)
        ambientVal = (float)strtof_wrap(value);

    /* _color (ambient RGB, normalized by max component) */
    float ambientColor[3] = {0.0f, 0.0f, 0.0f};
    {
        const char *ac = ValueForKey(entity, "_color");
        if (ac)
        {
            if (sscanf_wrap(ac, "%g %g %g", &ambientColor[0], &ambientColor[1], &ambientColor[2]) != 3 &&
                sscanf_wrap(ac, "%g, %g, %g", &ambientColor[0], &ambientColor[1], &ambientColor[2]) != 3 &&
                sscanf_wrap(ac, "( %g %g %g )", &ambientColor[0], &ambientColor[1], &ambientColor[2]) != 3 &&
                sscanf_wrap(ac, "( %g, %g, %g )", &ambientColor[0], &ambientColor[1], &ambientColor[2]) != 3)
                WarningMsg(2, "key '%s' has value '%s' which is not a valid vector\n", "_color", ac);
            else
            {
                float mc = ambientColor[0];
                if (ambientColor[1] > mc) mc = ambientColor[1];
                if (ambientColor[2] > mc) mc = ambientColor[2];
                if (mc != 0.0f) { float inv = 1.0f / mc; ambientColor[0] *= inv; ambientColor[1] *= inv; ambientColor[2] *= inv; }
            }
        }
    }

    /* compute per-channel ambient/diffuse/specular split with degamma correction.
     * LST 0x416132-0x416325 (ProcessEntity worldspawn lighting):
     *   rbp = &dword_480890 (base of contiguous light globals struct)
     *   [rbp + 0x38] = g_sunColorR/G/B        (specular component)
     *   [rbp + 0x44] = g_backfaceLightR/G/B   (diffuse component)
     *   [rbp + 0x50] = g_ambientR/G/B         (ambient component)
     * We write directly to the named globals — they are all overwritten from
     * the worldspawn startup defaults (0.7/0.3/0) once this function runs. */
    {
        extern float DegammaColorChannel(float color);
        extern float g_ambientR, g_ambientG, g_ambientB;
        float *ambientOut[3] = { &g_ambientR, &g_ambientG, &g_ambientB };
        float *radiosityOut[3] = { &g_backfaceLightR, &g_backfaceLightG, &g_backfaceLightB };
        float *directOut[3] = { &g_sunColorR, &g_sunColorG, &g_sunColorB };
        float *diffuseOut[3] = { &g_sunRadiosityR, &g_sunRadiosityG, &g_sunRadiosityB };
        float diffuseAmount;
        float directAmount;
        float radiosityAmount;
        int ch;

        if (ambientVal > sunIntensity)
        {
            WarningMsg(0, "WARNING: ambient %g > sunlight %g, increasing sunlight to match ambient\n",
                       (double)ambientVal, (double)sunIntensity);
            sunIntensity = ambientVal;
        }

        if (diffuseFrac < 0.0f || diffuseFrac > 1.0f)
        {
            WarningMsg(0, "WARNING: clamping diffuseFraction %g to the range [0, 1]\n",
                       (double)diffuseFrac);
            if (diffuseFrac < 0.0f)
                diffuseFrac = 0.0f;
            else
                diffuseFrac = 1.0f;
        }

        diffuseAmount = diffuseFrac * (sunIntensity - ambientVal);
        directAmount = sunIntensity - ambientVal - diffuseAmount;
        radiosityAmount = sunRadiosity - ambientVal - diffuseAmount;

        if (directAmount < 0.0f)
        {
            float required = ambientVal + diffuseAmount;
            WarningMsg(0, "WARNING: increasing sunlight %g to %g to match ambient + diffuse\n",
                       (double)sunIntensity, (double)required);
            sunIntensity = required;
            directAmount = 0.0f;
        }

        if (radiosityAmount < 0.0f)
        {
            float required = ambientVal + diffuseAmount;
            WarningMsg(0, "WARNING: increasing sunradiosity %g to %g to match ambient + diffuse\n",
                       (double)sunRadiosity, (double)required);
            sunRadiosity = required;
            radiosityAmount = 0.0f;
        }

        for (ch = 0; ch < 3; ch++)
        {
            float ambientCh = ambientVal * ambientColor[ch];
            float diffuseCh = diffuseAmount * sunDiffColor[ch];
            float directCh = directAmount * sunColor[ch];
            float radiosityCh = radiosityAmount * sunColor[ch];
            float directTotal = directCh + diffuseCh;
            float radiosityTotal = radiosityCh + diffuseCh;
            float ambientDegamma = DegammaColorChannel(ambientCh);

            *ambientOut[ch] = ambientDegamma;
            if (directTotal == 0.0f)
            {
                *directOut[ch] = 0.0f;
            }
            else
            {
                float delta = DegammaColorChannel(directTotal + ambientCh) - ambientDegamma;
                *directOut[ch] = delta * directCh / directTotal;
            }

            if (radiosityTotal == 0.0f)
            {
                *diffuseOut[ch] = 0.0f;
                *radiosityOut[ch] = 0.0f;
            }
            else
            {
                float delta = DegammaColorChannel(radiosityTotal + ambientCh) - ambientDegamma;
                /* Native globals 0x11622E8C and 0x11622E98 are easy to
                 * mislabel: E8C is the sun-radiosity component injected into
                 * the bounce ping buffer, while E98 is the diffuse sky field
                 * used by 0x406BA0/0x406A80. */
                *radiosityOut[ch] = delta * radiosityCh / radiosityTotal;
                *diffuseOut[ch] = delta * diffuseCh / radiosityTotal;
            }
        }
    }

    /* sundirection (vector, parsed and normalized) */
    {
        float sunDir[3] = {0.0f, 0.0f, 0.0f};
        const char *sdVal = ValueForKey(entity, "sundirection");
        if (sdVal)
        {
            if (sscanf_wrap(sdVal, "%g %g %g", &sunDir[0], &sunDir[1], &sunDir[2]) != 3 &&
                sscanf_wrap(sdVal, "%g, %g, %g", &sunDir[0], &sunDir[1], &sunDir[2]) != 3 &&
                sscanf_wrap(sdVal, "( %g %g %g )", &sunDir[0], &sunDir[1], &sunDir[2]) != 3 &&
                sscanf_wrap(sdVal, "( %g, %g, %g )", &sunDir[0], &sunDir[1], &sunDir[2]) != 3)
            {
                WarningMsg(2, "key '%s' has value '%s' which is not a valid vector\n", "sundirection", sdVal);
            }
            else
            {
                /* The original globals were adjacent in the x86 image, but
                 * separate C globals are not an array (and ASan deliberately
                 * places redzones between them).  Generate into a real vec3
                 * and then publish the three components. */
                float worldSunDir[3];
                extern void AngleVectors(float *inDir, float *outGlobal, int a3, int a4);
                AngleVectors(sunDir, worldSunDir, 0, 0);
                g_sunDirX = worldSunDir[0];
                g_sunDirY = worldSunDir[1];
                g_sunDirZ = worldSunDir[2];
            }
        }
    }

    /* zero first 3 ints of entity data (+0x00, +0x04, +0x08) */
    {
        int *entData = (int *)entity;
        entData[0] = 0;
        entData[1] = 0;
        entData[2] = 0;
    }

    /* validate model 0 exists */
    if (numBSPModels <= 0)
    {
        WarningMsg(1, "ignoring bad model index %i\n", 0);
        return;
    }

    /* get entity transform and iterate worldspawn (bspModels[0]) triangles */
    {
        float transform[12];
        int i;
        BspModel_t *world = &bspModels[0];

        GetEntityOriginAndAngles(entity, transform);

        for (i = 0; i < world->numTriSoups; i++)
        {
            AddTrianglesForSurface(&bspTriangles[world->firstTriSoup + i], 0, transform);
        }
    }
}

#define DEG_TO_RAD 0.017453292f  /* dword_459734 = 0x3C8EFA35 = pi/180 as float */

/*
 * ProcessLightEntity — process a light entity, creating point or spot light.
 * Address: 0x416990 | Size: 1768 bytes
 *
 * rcx=entity
 */
void ProcessLightEntity(Entity_t *entity)
{
    float origin[3];
    float color[3];
    float radius;
    float intensity;
    const char *defName;
    const char *targetValue;
    KeyValuePair_t *kv;

    /* assert: entity != NULL (line 0x2D) */
    Assert("entity", ".\\mapio.cpp", 0x2D, 0, 1);

    /* parse origin (inline, 4 format tries) */
    {
        const char *originStr = NULL;
        kv = entity->keyValues;
        while (kv) {
            if (I_stricmp(kv->key, "origin") == 0) { originStr = kv->value; break; }
            kv = kv->next;
        }
        if (!originStr)
            return;

        if (sscanf_wrap(originStr, "%g %g %g", &origin[0], &origin[1], &origin[2]) != 3 &&
            sscanf_wrap(originStr, "%g, %g, %g", &origin[0], &origin[1], &origin[2]) != 3 &&
            sscanf_wrap(originStr, "( %g %g %g )", &origin[0], &origin[1], &origin[2]) != 3 &&
            sscanf_wrap(originStr, "( %g, %g, %g )", &origin[0], &origin[1], &origin[2]) != 3)
        {
            WarningMsg(2, "key '%s' has value '%s' which is not a valid vector\n", "origin", originStr);
            return;
        }
    }

    /* look up "def" → light definition name */
    defName = "light_point_linear";
    kv = entity->keyValues;
    while (kv) {
        if (I_stricmp(kv->key, "def") == 0) {
            if (kv->value && kv->value[0])
                defName = kv->value;
            break;
        }
        kv = kv->next;
    }

    /* look up "radius" */
    radius = 0.0f;
    kv = entity->keyValues;
    while (kv) {
        if (I_stricmp(kv->key, "radius") == 0) {
            if (kv->value)
                radius = (float)strtof_wrap(kv->value);
            break;
        }
        kv = kv->next;
    }

    if (radius == 0.0f)
    {
        WarningMsg(1, "WARNING: ignoring light at (%.0f %.0f %.0f): no 'radius' key\n",
                         (double)origin[0], (double)origin[1], (double)origin[2]);
        return;
    }

    /* look up "_color" → parse RGB */
    {
        const char *colorStr = NULL;
        kv = entity->keyValues;
        while (kv) {
            if (I_stricmp(kv->key, "_color") == 0) { colorStr = kv->value; break; }
            kv = kv->next;
        }
        if (!colorStr)
            goto no_color;

        if (sscanf_wrap(colorStr, "%g %g %g", &color[0], &color[1], &color[2]) != 3 &&
            sscanf_wrap(colorStr, "%g, %g, %g", &color[0], &color[1], &color[2]) != 3 &&
            sscanf_wrap(colorStr, "( %g %g %g )", &color[0], &color[1], &color[2]) != 3 &&
            sscanf_wrap(colorStr, "( %g, %g, %g )", &color[0], &color[1], &color[2]) != 3)
        {
            WarningMsg(2, "key '%s' has value '%s' which is not a valid vector\n", "_color", colorStr);
        no_color:
            WarningMsg(1, "WARNING: ignoring light at (%.0f %.0f %.0f): no '_color' key\n",
                             (double)origin[0], (double)origin[1], (double)origin[2]);
            return;
        }
    }

    /* look up "intensity" → default 1.0 */
    intensity = 1.0f;
    kv = entity->keyValues;
    while (kv) {
        if (I_stricmp(kv->key, "intensity") == 0) {
            if (kv->value) {
                float parsed = (float)strtof_wrap(kv->value);
                if (parsed > 0.0f)
                    intensity = parsed;
            }
            break;
        }
        kv = kv->next;
    }

    /* normalize color by max component, multiply by intensity */
    {
        float maxComp = color[0];
        if (color[1] > maxComp) maxComp = color[1];
        if (color[2] > maxComp) maxComp = color[2];

        if (maxComp != 0.0f)
        {
            float invMax = 1.0f / maxComp;
            color[0] *= invMax;
            color[1] *= invMax;
            color[2] *= invMax;
        }
        color[0] *= intensity;
        color[1] *= intensity;
        color[2] *= intensity;
    }

    /* check for "target" key → spot light */
    targetValue = ValueForKey(entity, "target");
    if (!targetValue)
    {
        /* no target → point light */
        AddPointLight(0, origin, radius, color, defName);
        return;
    }

    /* find target entity */
    {
        Entity_t *targetEnt = FindEntityWithKeyValue("targetname", targetValue);
        float targetOrigin[3];
        float dir[3];
        float outerAngle, innerAngle;
        float outerCos, innerCos;
        int exponent;
        float dist;

        if (!targetEnt)
        {
            WarningMsg(1, "WARNING: ignoring spotlight at (%.0f %.0f %.0f): target entity '%s' not found\n",
                             (double)origin[0], (double)origin[1], (double)origin[2], targetValue);
            return;
        }

        /* get target origin */
        if (!ParseVectorString(targetEnt, "origin", targetOrigin))
            return;

        /* compute direction = origin - targetOrigin, normalize */
        dir[0] = origin[0] - targetOrigin[0];
        dir[1] = origin[1] - targetOrigin[1];
        dir[2] = origin[2] - targetOrigin[2];
        dist = Vec3Normalize(dir);

        /* look up fov_outer */
        outerAngle = 0.0f;
        kv = entity->keyValues;
        while (kv) {
            if (I_stricmp(kv->key, "fov_outer") == 0) {
                if (kv->value) outerAngle = (float)strtof_wrap(kv->value);
                break;
            }
            kv = kv->next;
        }

        if (outerAngle == 0.0f)
        {
            /* default: compute from dist */
            outerCos = dist / sqrtf(dist * dist + 4096.0f); /* dword_459724 = 4096.0f */
        }
        else
        {
            { extern float x87_cosf(float); outerCos = x87_cosf(outerAngle * DEG_TO_RAD * 0.5f); }
        }

        /* look up fov_inner → default 0 */
        innerAngle = 0.0f;
        kv = entity->keyValues;
        while (kv) {
            if (I_stricmp(kv->key, "fov_inner") == 0) {
                if (kv->value) innerAngle = (float)strtof_wrap(kv->value);
                break;
            }
            kv = kv->next;
        }

        { extern float x87_cosf(float); innerCos = x87_cosf(innerAngle * DEG_TO_RAD * 0.5f); }

        if (outerCos > innerCos)
        {
            WarningMsg(1, "WARNING: ignoring spotlight at (%.0f %.0f %.0f): fov_inner > fov_outer\n",
                             (double)origin[0], (double)origin[1], (double)origin[2]);
            return;
        }

        /* look up exponent → default 0 */
        exponent = 0;
        kv = entity->keyValues;
        while (kv) {
            if (I_stricmp(kv->key, "exponent") == 0) {
                if (kv->value) exponent = atoi_wrap(kv->value);
                break;
            }
            kv = kv->next;
        }

        AddSpotLight(0, origin, radius, color, defName, dir, outerCos, innerCos,
                     exponent);
    }
}
/* strtof_wrap declared at top of file */

/*
 * ProcessBrushModel — process a misc_model entity.
 * Address: 0x4166A0 | Size: 749 bytes
 *
 * rcx=entity
 * Loads the xmodel, gets transform, calls CM_TraceBox for collision,
 * adds ground lighting and light grid samples.
 */
void ProcessBrushModel(Entity_t *entity)
{
    const char *modelValue;
    const char *modelName;
    void *xmodel;
    float transform[12]; /* origin[3] + axis[9] */
    float scale[3];
    float center[3];
    float boxHeight = 10.0f; /* dword_456494: default box extent, may be overwritten by CM_TraceBox */
    int spawnFlags;
    KeyValuePair_t *kv;
    char xmodelPath[128];
    float modelScale;

    /* assert: entity != NULL (line 0x2D) */
    Assert("entity", ".\\mapio.cpp", 0x2D, 0, 1);

    /* check spawnflags bit 1 — skip if set */
    spawnFlags = 0;
    kv = entity->keyValues;
    while (kv)
    {
        if (I_stricmp(kv->key, "spawnflags") == 0)
        {
            if (kv->value)
                spawnFlags = atoi_wrap(kv->value);
            break;
        }
        kv = kv->next;
    }

    /* Retail CoD4Rad sub_418030 skips both NO_SHADOW (2) and
     * NO_STATIC_SHADOWS (4). */
    if (spawnFlags & (2 | 4)) return;

    /* look up "model" key */
    modelValue = NULL;
    kv = entity->keyValues;
    while (kv)
    {
        if (I_stricmp(kv->key, "model") == 0)
        {
            modelValue = kv->value;
            break;
        }
        kv = kv->next;
    }

    if (!modelValue) return;

    /* CoD4Rad sub_418030 uses sub_44B180 only to recognize and strip an
     * optional "xmodel/" (or "xmodel\\") prefix.  Entity lumps normally
     * contain bare asset names; rejecting them empties both the static-model
     * shadow trace world and the model-origin light-grid source. */
    modelName = modelValue;
    if (strncmp(modelValue, "xmodel", 6) == 0
        && (modelValue[6] == '/' || modelValue[6] == '\\'))
    {
        modelName += 7;
    }

    if (!modelName[0]) return;

    /* build xmodel path: "shadow_" + bare model name */
    {
        char *dst;
        const char *src;
        int i;

        /* write "shadow_" prefix */
        *(long long *)xmodelPath = 0x5F776F64616873LL; /* "shadow_\0" */

        /* find end of prefix */
        dst = xmodelPath;
        while (*dst) dst++;

        /* copy the bare model name */
        src = modelName;
        i = 0;
        do {
            dst[i] = src[i];
        } while (src[i++]);
    }

    /* try loading shadow model first, then original.  KISAK: probe the shadow_
     * variant's existence QUIETLY first — only a handful of stock models ship one,
     * and letting XModelPrecache miss on every other model spammed
     * "^1ERROR: xmodel 'shadow_...' not found" per placed misc_model.
     * FS_ReadFile with a NULL buffer returns the length without reading. */
    xmodel = NULL;
    {
        char probePath[1024];
        if (Com_sprintf(probePath, sizeof(probePath), "xmodel/%s", xmodelPath) >= 0
            && FS_ReadFile(probePath, NULL) >= 0)
        {
            xmodel = XModelPrecache(xmodelPath, XModel_AllocZeroed, XModel_AllocZeroed);
        }
    }
    if (!xmodel)
    {
        xmodel = XModelPrecache(modelName, XModel_AllocZeroed, XModel_AllocZeroed);
        if (!xmodel)
        {
            WarningMsg(1, "failed to load misc_model '%s'\n", modelName);
            goto done;
        }
    }

    /* get entity transform */
    GetEntityOriginAndAngles(entity, transform);

    /* look up modelscale */
    modelScale = 1.0f;
    kv = entity->keyValues;
    while (kv)
    {
        if (I_stricmp(kv->key, "modelscale") == 0)
        {
            if (kv->value)
            {
                float parsed = (float)strtof_wrap(kv->value);
                if (parsed != 0.0f)
                    modelScale = parsed;
            }
            break;
        }
        kv = kv->next;
    }

    /* build scale vector */
    scale[0] = modelScale;
    scale[1] = modelScale;
    scale[2] = modelScale;

    /* call CM_TraceBox: get center and box height */
    {
        /* boxHeight already initialized to 10.0f at declaration */
        CM_TraceBox(xmodel, scale, transform, center, &boxHeight);
    }

    /* add light grid samples */
    AddStaticModelLightGridSample((int *)center);
    AddStaticModelLightGridSample((int *)transform);

    /* add ground lighting — direction is third axis row (transform[9..11]) */
    AddStaticModelToGroundLitList(entity, center, boxHeight, &transform[9]);

done:
    return;
}

/*
 * GetEntityOriginAndAngles — extract origin and build rotation matrix from entity.
 * Address: 0x4156E0 | Size: 675 bytes
 *
 * rcx=entity, rdx=outTransform (float[12]: origin[3] + axis[9])
 *
 * Looks up "origin" key → parses to outTransform[0..2].
 * Looks up "angles" key → parses to local, calls AnglesToAxis → outTransform[3..11].
 * Uses same 4-format sscanf pattern as ParseVectorString.
 */
void GetEntityOriginAndAngles(Entity_t *entity, float *outTransform)
{
    const char *originStr;
    const char *anglesStr;
    float angles[3];
    KeyValuePair_t *kv;

    /* assert: entity != NULL (line 0x2D) */
    Assert("entity", ".\\mapio.cpp", 0x2D, 0, 1);

    /* look up "origin" key inline */
    originStr = NULL;
    kv = entity->keyValues;
    while (kv)
    {
        if (I_stricmp(kv->key, "origin") == 0)
        {
            originStr = kv->value;
            break;
        }
        kv = kv->next;
    }

    if (!originStr)
        goto no_origin;

    /* parse origin with 4 format tries */
    if (sscanf_wrap(originStr, "%g %g %g", &outTransform[0], &outTransform[1], &outTransform[2]) == 3)
        goto parse_angles;
    if (sscanf_wrap(originStr, "%g, %g, %g", &outTransform[0], &outTransform[1], &outTransform[2]) == 3)
        goto parse_angles;
    if (sscanf_wrap(originStr, "( %g %g %g )", &outTransform[0], &outTransform[1], &outTransform[2]) == 3)
        goto parse_angles;
    if (sscanf_wrap(originStr, "( %g, %g, %g )", &outTransform[0], &outTransform[1], &outTransform[2]) == 3)
        goto parse_angles;

    WarningMsg(2, "key '%s' has value '%s' which is not a valid vector\n", "origin", originStr);

no_origin:
    outTransform[0] = 0.0f;
    outTransform[1] = 0.0f;
    outTransform[2] = 0.0f;

parse_angles:
    /* look up "angles" key inline */
    anglesStr = NULL;
    kv = entity->keyValues;
    while (kv)
    {
        if (I_stricmp(kv->key, "angles") == 0)
        {
            anglesStr = kv->value;
            break;
        }
        kv = kv->next;
    }

    if (!anglesStr)
        goto no_angles;

    /* parse angles with 4 format tries */
    if (sscanf_wrap(anglesStr, "%g %g %g", &angles[0], &angles[1], &angles[2]) == 3)
        goto build_axis;
    if (sscanf_wrap(anglesStr, "%g, %g, %g", &angles[0], &angles[1], &angles[2]) == 3)
        goto build_axis;
    if (sscanf_wrap(anglesStr, "( %g %g %g )", &angles[0], &angles[1], &angles[2]) == 3)
        goto build_axis;
    if (sscanf_wrap(anglesStr, "( %g, %g, %g )", &angles[0], &angles[1], &angles[2]) == 3)
        goto build_axis;

    WarningMsg(2, "key '%s' has value '%s' which is not a valid vector\n", "angles", anglesStr);

no_angles:
    angles[0] = 0.0f;
    angles[1] = 0.0f;
    angles[2] = 0.0f;

build_axis:
    AnglesToAxis(angles, &outTransform[3]);
}

/*
 * ProcessEntities — dispatch entity processing based on classname.
 * Address: 0x417080 | Size: 396 bytes
 *
 * rcx=entity
 * Looks up classname, dispatches to appropriate processor.
 */
void ProcessEntities(Entity_t *entity)
{
    const char *classname;
    const char *modelValue;

    /* assert: entity != NULL (line 0x2D) */
    Assert("entity", ".\\mapio.cpp", 0x2D, 0, 1);

    /* look up classname */
    classname = ValueForKey(entity, "classname");

    /* assert: classname found (line 0x1A7) */
    Assert("classname", ".\\mapio.cpp", 0x1A7, 0, 1);

    /* dispatch based on classname */
    if (I_stricmp(classname, "worldspawn") == 0)
    {
        ProcessEntity(entity);
        return;
    }

    if (I_stricmp(classname, "misc_model") == 0)
    {
        ProcessBrushModel(entity);
        return;
    }

    if (I_stricmp(classname, "script_model") == 0)
    {
        /* Retail dispatches sub_418030(entity, 1) and carries that flag into
         * sub_41A990's collision instance.  The x64 collision record does not
         * yet model the script-only triangle-test semantics, so do not fold it
         * silently into misc_model behavior. */
        WarningMsg(1,
            "WARNING: script_model static-shadow baking is not implemented; skipping model collision/light-grid seeding for this entity.\n");
        return;
    }

    if (I_stricmp(classname, "light") == 0)
    {
        ProcessLightEntity(entity);
        return;
    }

    /* check for brush model entity (model key starts with '*') */
    modelValue = ValueForKey(entity, "model");
    if (modelValue && modelValue[0] == '*')
    {
        ProcessBrushModelTriangles(entity, modelValue);
    }
}

/*
 * Map_Write — finalize lightmaps and write BSP file.
 * Address: 0x417310 | Size: 43 bytes
 *
 * rcx=filename
 */
void Map_Write(const char *filename)
{
    BuildFinalLightmaps_TripleLoop();
    UnparseEntities();
    WriteBspFile(filename, g_targetPlatform->bigEndian);
}

/*
 * Map_Read — load materials, process entities, allocate lightmap data.
 * Address: 0x417210 | Size: 252 bytes
 *
 * rcx=filename
 * Returns 1 (al).
 */
extern void InitBSP(void);
extern void LoadBSPShaders(void);
extern void InitCollision(void);
/* LoadMaterial — now in materials.c */
/* Map_ReadPolyFile — now in polyfile.c */

char Map_Read(const char *filename)
{
    int i;

    LoadBspFile(filename);
    ParseEntities();
    InitGeometry_Reset();

    /* load all materials */
    for (i = 0; i < numBSPMaterials; i++)
    {
        char *materialName = bspMaterials[i].material;

        /* skip "noshader" */
        if (I_stricmp(materialName, "noshader") == 0)
            continue;

        if (!LoadMaterial(materialName))
        {
            WarningMsg(1, "couldn't load material '%s'\n", materialName);
        }
    }

    /* process all entities */
    for (i = 0; i < num_entities; i++)
    {
        ProcessEntities(&g_entities[i]);
    }

    /* allocate lightmap data */
    Lighting_AllocLightmapData();

    /* start light calculation */
    Map_ReadPolyFile(filename);

    return 1;
}

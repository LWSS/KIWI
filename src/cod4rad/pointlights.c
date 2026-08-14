/*
 * pointlights.c — Point and spot light loading and evaluation.
 */

#include "cod2rad64.h"

LightDef_t   g_lightDefs[MAX_RAD_LIGHTDEFS];
int          g_numLightDefs;
PointLight_t g_pointLights[MAX_RAD_POINTLIGHTS];
int          g_numPointLights;

/* external functions not in master header */
extern int LoadLightDefImages(const char *name, int *outType,
                              ImageDecodeState_t *outImage2, ImageDecodeState_t *outImage1);

/*
================
LoadLightDef

Find or load a light definition by name.
================
*/
LightDef_t *LoadLightDef(const char *name)
{
    int i;
    LightDef_t *def;

    for (i = 0; i < g_numLightDefs; i++)
    {
        if (strcmp(g_lightDefs[i].name, name) == 0)
            return &g_lightDefs[i];
    }

    if (g_numLightDefs == 64)
    {
        Com_Printf("More than %i lightDefs used by all lights in map\n",
                    g_numLightDefs, name);
    }

    /* allocate new slot */
    def = &g_lightDefs[g_numLightDefs];

    /* load images */
    if (!LoadLightDefImages(name, &def->image1, def->falloffName, &def->image2))
    {
        Com_Printf("Couldn't get light def images for '%s'\n", name);
    }

    /* validate falloff image type */
    if (def->type != 1)
    {
        Com_Printf("Falloff image %s in light def %s has dimension %i instead of 1\n",
                    def->falloffName, name, def->dimension, def->type);
    }

    /* intern the name string */
    def->name = _strdup(name);

    g_numLightDefs++;

    return def;
}

/*
================
AddPointLight

Add a point light to the global light array.
================
*/
void AddPointLight(float *origin, float radius, float *color, const char *defName)
{
    int idx;
    PointLight_t *light;
    LightDef_t *def;

    idx = g_numPointLights;
    if (idx == 0x800)
    {
        Com_Printf("More than %i lights in map\n", idx);
        idx = g_numPointLights;
    }

    /* load light def */
    def = LoadLightDef(defName);

    light = &g_pointLights[idx];
    light->def = def;

    if (!def)
        return;

    /* store origin */
    light->origin[0] = origin[0];
    light->origin[1] = origin[1];
    light->origin[2] = origin[2];

    /* store radius */
    light->radius = radius;

    /* compute falloff scale: def->dimension / radius */
    light->falloffScale = (float)def->dimension / radius;

    /* store and degamma color */
    light->color[0] = color[0];
    light->color[1] = color[1];
    light->color[2] = color[2];
    DegammaColor(light->color);

    light->isSpot = 0;
    g_numPointLights++;
}

/*
================
AddSpotLight

Add a spot light to the global light array.
================
*/
void AddSpotLight(float *origin, float radius, float *color, const char *defName,
                  float *dir, float outerCosAngle, float innerCosAngle, int exponent)
{
    int idx;
    PointLight_t *light;
    LightDef_t *def;
    float invRange;

    idx = g_numPointLights;
    if (idx == 0x800)
    {
        Com_Printf("More than %i lights in map\n", idx);
        idx = g_numPointLights;
    }

    /* load light def */
    def = LoadLightDef(defName);

    light = &g_pointLights[idx];
    light->def = def;

    if (!def)
        return;

    /* store origin */
    light->origin[0] = origin[0];
    light->origin[1] = origin[1];
    light->origin[2] = origin[2];

    /* store radius */
    light->radius = radius;

    /* compute falloff scale: def->dimension / radius */
    light->falloffScale = (float)def->dimension / radius;

    /* store and degamma color */
    light->color[0] = color[0];
    light->color[1] = color[1];
    light->color[2] = color[2];
    DegammaColor(light->color);

    /* mark as spot light */
    light->isSpot = 0;
    g_numPointLights++;
    light->isSpot = 1;

    /* compute spot light parameters */
    invRange = 1.0f / (innerCosAngle - outerCosAngle);

    light->spotDir[0] = dir[0];
    light->spotDir[1] = dir[1];
    light->spotDir[2] = dir[2];

    light->spotScale = invRange;
    light->spotCosAngle = outerCosAngle;
    light->spotOffset = -0.0f - outerCosAngle * invRange;
    light->spotExponent = exponent;
}

/*
================
GetPointLightCount

Return current point light count.
================
*/
int GetPointLightCount(void)
{
    return g_numPointLights;
}

/*
================
PointLightEvaluatePoint

Evaluate a point/spot light's contribution at a position.
Returns 1 if light contributes, 0 if not.
================
*/
extern int TraceVisibility(int flags, float *traceStart, PointLight_t *light);

int PointLightEvaluatePoint(int flags, int lightIndex, float *pos, float *normal,
                            float *outDir, float *outColor, float *outDot)
{
    PointLight_t *light;
    float dx, dy, dz;
    float distSq, dist;
    float falloffIdx;
    int falloffIdxInt;
    LightDef_t *def;
    float invDist;
    float dot;
    float spotAtten;
    float spotDot;
    float falloffR, falloffG, falloffB;
    float frac, invFrac;
    unsigned char *texData;
    float traceStart[3];
    float scale_255 = 1.0f / 255.0f;
    int result = 1; /* edi: return value, 1=far hit, 2=near hit */

    /* assert: lightIndex in range (line 0x95) */
    Assert("(lightIndex >= 0 && lightIndex < pointLightCount)",
           ".\\pointlights.cpp", 0x95, 0, 1);

    light = &g_pointLights[lightIndex];

    /* assert: light->def != NULL (line 0x98) */
    Assert("light->def", ".\\pointlights.cpp", 0x98, 0, 1);

    /* assert: light->def->type == 2 (line 0x99) */
    Assert("(light->def->type == GFX_LIGHT_TYPE_POINT)",
           ".\\pointlights.cpp", 0x99, 0, 1);

    /* compute direction vector from light to pos */
    dx = light->origin[0] - pos[0];
    dy = light->origin[1] - pos[1];
    dz = light->origin[2] - pos[2];

    /* distance squared */
    distSq = dx * dx + dy * dy + dz * dz;

    /* early out: outside radius */
    if (distSq > light->radius * light->radius)
        return 0;

    /* compute distance and falloff index */
    dist = sqrtf(distSq);
    falloffIdx = dist * light->falloffScale - 0.5f;
    falloffIdxInt = (int)floorf(falloffIdx);

    def = light->def;

    /* if at last texel, no contribution */
    if (falloffIdxInt == def->dimension - 1)
        return 0;

    if (dist < 0.001f)
    {
        /* very close: full intensity, zero direction */
        outDir[0] = 0;
        outDir[1] = 0;
        outDir[2] = 0;
        if (outDot)
            *outDot = 1.0f;

        result = 2; /* near hit returns 2 */
        spotAtten = 1.0f; /* xmm6 = 1.0 */

        /* use first texel directly if falloffIdx < 0 */
        if (falloffIdxInt < 0)
        {
            texData = (unsigned char *)def->data;
            falloffR = (float)texData[0] * scale_255;
            falloffG = (float)texData[1] * scale_255;
            falloffB = (float)texData[2] * scale_255;
            goto apply_color;
        }
    }
    else
    {
        /* normalize direction */
        invDist = 1.0f / dist;
        dx *= invDist;
        dy *= invDist;
        dz *= invDist;
        outDir[0] = dx;
        outDir[1] = dy;
        outDir[2] = dz;

        /* dot product with normal if provided */
        if (normal)
        {
            dot = dx * normal[0] + dy * normal[1] + dz * normal[2];
            if (dot < 0.0f)
                return 0;
            if (outDot)
                *outDot = dot;
        }

        /* spot light cone check */
        spotAtten = 1.0f;
        if (light->isSpot)
        {
            spotDot = outDir[0] * light->spotDir[0]
                    + outDir[1] * light->spotDir[1]
                    + outDir[2] * light->spotDir[2];

            if (spotDot <= light->spotCosAngle)
                return 0;

            /* compute spot attenuation */
            spotAtten = spotDot * light->spotScale + light->spotOffset;
            if (spotAtten >= 1.0f)
            {
                spotAtten = 1.0f;
            }
            else
            {
                /* apply spot exponent: spotAtten = spotAtten^exponent */
                /* unrolled power loop: 8x multiply per outer iteration */
                int exp = light->spotExponent;
                int done = 0;
                float base = spotAtten;

                spotAtten = 1.0f; /* xmm6 starts at 1.0, gets powered */
                if (exp >= 8)
                {
                    int chunks = ((exp - 8) >> 3) + 1;
                    done = chunks * 8;
                    while (chunks > 0)
                    {
                        spotAtten = spotAtten * base;
                        spotAtten *= base;
                        spotAtten *= base;
                        spotAtten *= base;
                        spotAtten *= base;
                        spotAtten *= base;
                        spotAtten *= base;
                        spotAtten *= base;
                        chunks--;
                    }
                }
                if (done < exp)
                {
                    int rem = exp - done;
                    while (rem > 0)
                    {
                        spotAtten *= base;
                        rem--;
                    }
                }
            }
        }

        /* shadow/visibility trace (both spot and non-spot) */
        {
            float traceOffset = 0.125f;
            traceStart[0] = outDir[0] * traceOffset + pos[0];
            traceStart[1] = outDir[1] * traceOffset + pos[1];
            traceStart[2] = outDir[2] * traceOffset + pos[2];

            if (!TraceVisibility(flags, traceStart, light))
                return 0;

            if (normal)
            {
                if (TraceStaticModels(traceStart, (float *)light))
                    return 0;
            }
        }
    }

do_falloff_lookup:
    /* falloff texture bilinear interpolation */
    {
        long long idx = (long long)falloffIdxInt;
        frac = falloffIdx - (float)falloffIdxInt;
        invFrac = 1.0f - frac;
        texData = (unsigned char *)def->data;

        /* interpolate R: texData[idx*4+4]*frac + texData[idx*4+0]*invFrac */
        falloffR = ((float)texData[idx * 4 + 4] * frac
                  + (float)texData[idx * 4 + 0] * invFrac) * scale_255;

        /* interpolate G: texData[idx*4+5]*frac + texData[idx*4+1]*invFrac */
        falloffG = ((float)texData[idx * 4 + 5] * frac
                  + (float)texData[idx * 4 + 1] * invFrac) * scale_255;

        /* interpolate B: texData[idx*4+6]*frac + texData[idx*4+2]*invFrac */
        falloffB = ((float)texData[idx * 4 + 6] * frac
                  + (float)texData[idx * 4 + 2] * invFrac) * scale_255;
    }

apply_color:
    /* multiply falloff by light color, then by spot attenuation */
    falloffR *= light->color[0];
    falloffG *= light->color[1];
    falloffB *= light->color[2];

    outColor[0] = spotAtten * falloffR;
    outColor[1] = spotAtten * falloffG;
    outColor[2] = spotAtten * falloffB;

    return result;
}

/*
 * pointlights.c — Point and spot light loading and evaluation.
 */

#include "cod2rad64.h"
#include <stdlib.h>
#include <string.h>

LightDef_t   g_lightDefs[MAX_RAD_LIGHTDEFS];
int          g_numLightDefs;
PointLight_t g_pointLights[MAX_RAD_POINTLIGHTS];
int          g_numPointLights;

/*
================
LoadLightDef

Find or load a light definition by name.
================
*/
LightDef_t *LoadLightDef(const char *name)
{
    ImageDecodeState_t image;
    int i;
    int pixelCount;
    LightDef_t *def;

    for (i = 0; i < g_numLightDefs; i++)
    {
        if (strcmp(g_lightDefs[i].name, name) == 0)
            return &g_lightDefs[i];
    }

    if (g_numLightDefs == MAX_RAD_LIGHTDEFS)
    {
        Com_Printf("More than %i lightDefs used by all lights combined; can't load '%s'\n",
                    g_numLightDefs, name);
        return NULL;
    }

    /* allocate new slot */
    def = &g_lightDefs[g_numLightDefs];

    memset(&image, 0, sizeof(image));
    if (!LoadLightDefImages(name, &image))
    {
        Com_Printf("Couldn't get light def images for '%s'\n", name);
        return NULL;
    }

    if (image.height != 1)
    {
        Com_Printf("Falloff image %s in light def %s has dimensions %ix%i; height should be 1\n",
                    image.name, name, image.stride, image.height);
    }

    def->name = _strdup(name);
    def->width = image.stride;
    def->height = image.height;
    pixelCount = def->width * def->height;
    def->data = (float *)malloc((size_t)pixelCount * 3 * sizeof(*def->data));
    if (!def->data)
        Error("Couldn't allocate light def image data for '%s'\n", name);

    /* Native sub_41AF80 stores degamma-corrected RGB floats, three per
     * attenuation texel.  Alpha is not part of point-light falloff. */
    for (i = 0; i < pixelCount; ++i)
    {
        def->data[i * 3 + 0] = DegammaColorChannel((float)image.pixels[i * 4 + 0] * (1.0f / 255.0f));
        def->data[i * 3 + 1] = DegammaColorChannel((float)image.pixels[i * 4 + 1] * (1.0f / 255.0f));
        def->data[i * 3 + 2] = DegammaColorChannel((float)image.pixels[i * 4 + 2] * (1.0f / 255.0f));
    }
    Z_FreeInternal(image.pixels);

    g_numLightDefs++;

    return def;
}

/*
================
AddPointLight

Add a point light to the global light array.
================
*/
void AddPointLight(int primaryLightIndex, float *origin, float radius,
                   float *color, const char *defName)
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

    light->primaryLightIndex = primaryLightIndex ? primaryLightIndex : -1;

    /* store origin */
    light->origin[0] = origin[0];
    light->origin[1] = origin[1];
    light->origin[2] = origin[2];

    /* store radius */
    light->radius = radius;

    /* compute falloff scale: def->width / radius */
    light->falloffScale = (float)def->width / radius;

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
void AddSpotLight(int primaryLightIndex, float *origin, float radius,
                  float *color, const char *defName, float *dir,
                  float outerCosAngle, float innerCosAngle, int exponent)
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

    light->primaryLightIndex = primaryLightIndex ? primaryLightIndex : -1;

    /* store origin */
    light->origin[0] = origin[0];
    light->origin[1] = origin[1];
    light->origin[2] = origin[2];

    /* store radius */
    light->radius = radius;

    /* compute falloff scale: def->width / radius */
    light->falloffScale = (float)def->width / radius;

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
    /* KIWI FIX: fov_inner == fov_outer (a hard-edged cone) made this 1/0 = inf, and then
       spotAtten = spotDot*inf - outerCos*inf = NaN for every point inside the cone - the
       same NaN-energy sanity check as a negative colour.  Treat a zero-width band as a
       very steep one: the attenuation clamps to 1 just inside the edge. */
    {
        float range = innerCosAngle - outerCosAngle;
        if (range < 1.0e-6f)
            range = 1.0e-6f;
        invRange = 1.0f / range;
    }

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
extern int TraceVisibility(int cacheIndex, float *startPos, float *endPos);   /* geometry.c:940 */

int PointLightEvaluatePoint(int surfacePrimaryLightIndex, int traceIndex,
                            int lightIndex, float *pos, float *normal,
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
    float *texData;
    float traceStart[3];
    int result;

    /* assert: lightIndex in range (line 0x95) */
    Assert("(lightIndex >= 0 && lightIndex < pointLightCount)",
           ".\\pointlights.cpp", 0x95, 0, 1);

    light = &g_pointLights[lightIndex];
    result = (light->primaryLightIndex != surfacePrimaryLightIndex) + 1;

    /* assert: light->def != NULL (line 0x98) */
    Assert("light->def", ".\\pointlights.cpp", 0x98, 0, 1);

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
    if (falloffIdxInt == def->width - 1)
        return 0;

    if (dist < 0.001f)
    {
        /* very close: full intensity, zero direction */
        outDir[0] = 0;
        outDir[1] = 0;
        outDir[2] = 0;
        if (outDot)
            *outDot = 1.0f;

        result = 2 * (light->primaryLightIndex != surfacePrimaryLightIndex) + 1;
        spotAtten = 1.0f; /* xmm6 = 1.0 */

        /* use first texel directly if falloffIdx < 0 */
        if (falloffIdxInt < 0)
        {
            texData = def->data;
            falloffR = texData[0];
            falloffG = texData[1];
            falloffB = texData[2];
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

            /* KIWI FIX: PointLight_t gained a leading primaryLightIndex, so the old
               (float *)light casts handed the traces {int-as-float, origin.x, origin.y}
               as the endpoint -- point lights were effectively unshadowed.  Trace to
               the light's origin explicitly. */
            if (!TraceVisibility(traceIndex, traceStart, light->origin))
                return 0;

            if (normal)
            {
                if (TraceStaticModels(traceStart, light->origin))
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
        texData = def->data;

        falloffR = texData[idx * 3 + 3] * frac
                 + texData[idx * 3 + 0] * invFrac;
        falloffG = texData[idx * 3 + 4] * frac
                 + texData[idx * 3 + 1] * invFrac;
        falloffB = texData[idx * 3 + 5] * frac
                 + texData[idx * 3 + 2] * invFrac;
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

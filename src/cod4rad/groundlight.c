/*
 * groundlight.c — Ground lighting calculation for static models
 */

#include "cod2rad64.h"
#include <stdlib.h>

extern int FindLightingSamplesAndNormal(int sampleIdx, float *position, float *normal,
                                        float offset, void *outputLighting,
                                        float *outputNormal);
extern void Lighting_GetGatheredLight(LightingSample_t *sample, float *outVars);
extern unsigned char EncodeFloatInByte(float value);
extern float GammaCorrectColorChannel(float value);
extern int Com_sprintf(char *buf, int size, const char *fmt, ...);

GroundLitModel_t *g_groundLitList;

static char s_assertDisable_hitSample;
static char s_assertDisable_hitSampleVars;

/*
================
AddStaticModelToGroundLitList

Add a static model to the ground lighting list.
================
*/
void AddStaticModelToGroundLitList(void *entity, float *pos, float radius, float *dir)
{
    GroundLitModel_t *node;

    node = (GroundLitModel_t *)malloc(sizeof(GroundLitModel_t));

    node->entity = entity;
    node->pos[0] = pos[0];
    node->pos[1] = pos[1];
    node->pos[2] = pos[2];
    node->dir[0] = dir[0];
    node->dir[1] = dir[1];
    node->dir[2] = dir[2];
    node->radius = radius;

    node->next = g_groundLitList;
    g_groundLitList = node;
}

/*
================
CalculateGroundLighting_Worker

Calculate ground lighting for a single model.
Computes a sample position offset by dir*radius*0.5, traces for lighting,
accumulates weighted color and intensity, applies gamma correction.
Returns 1 if lighting found, 0 otherwise.
================
*/
int CalculateGroundLighting_Worker(float *pos, float radius, float *dir,
                                    float *outColor, float *outIntensity)
{
    float samplePos[3];
    float sampleDir[3];
    float normals[4];
    LightingHit_t hits[4];
    float halfRadius;
    float maxDist;
    int hitCount;
    int i;
    float sunDot;
    float lightVars[3];
    float intensity;

    halfRadius = radius * 0.5f;
    maxDist = radius + 4.0f;

    samplePos[0] = dir[0] * halfRadius + pos[0];
    samplePos[1] = dir[1] * halfRadius + pos[1];
    samplePos[2] = dir[2] * halfRadius + pos[2];

    sampleDir[0] = -dir[0];
    sampleDir[1] = -dir[1];
    sampleDir[2] = -dir[2];

    hitCount = FindLightingSamplesAndNormal(0, samplePos, sampleDir, maxDist,
                                            hits, normals);

    if (hitCount == 0 || hitCount == -1)
        return 0;

    sunDot = normals[0] * g_sunDirX
           + normals[1] * g_sunDirY
           + normals[2] * g_sunDirZ;

    if (sunDot < 0.0f) sunDot = 0.0f;
    if (sunDot > 1.0f) sunDot = 1.0f;

    outColor[0] = 0.0f;
    outColor[1] = 0.0f;
    outColor[2] = 0.0f;
    *outIntensity = 0.0f;

    if (hitCount <= 0)
        goto after_accumulate;

    for (i = 0; i < hitCount; i++)
    {
        LightingHit_t *hit = &hits[i];
        float weight;

        Assert(hit->sample, s_assertDisable_hitSample);
        Assert(hit->sample->vars, s_assertDisable_hitSampleVars);

        Lighting_GetGatheredLight(hit->sample, lightVars);

        weight = hit->weight;

        outColor[0] += lightVars[0] * weight;
        outColor[1] += lightVars[1] * weight;
        outColor[2] += lightVars[2] * weight;

        {
            float *varPtr = hit->sample->vars->intensity;
            int k;
            for (k = 0; k < 2; k++)
            {
                int j;
                for (j = 0; j < 2; j++)
                {
                    *outIntensity += varPtr[0] * weight;
                    varPtr++;
                }
            }
        }
    }

after_accumulate:
    *outIntensity *= 0.25f;

    intensity = GammaCorrectColorChannel(*outIntensity);
    intensity *= sunDot;

    if (intensity < 0.0f) intensity = 0.0f;
    if (intensity > 1.0f) intensity = 1.0f;
    *outIntensity = intensity;

    outColor[0] = GammaCorrectColorChannel(outColor[0]);
    if (outColor[0] < 0.0f) outColor[0] = 0.0f;
    if (outColor[0] > 1.0f) outColor[0] = 1.0f;

    outColor[1] = GammaCorrectColorChannel(outColor[1]);
    if (outColor[1] < 0.0f) outColor[1] = 0.0f;
    if (outColor[1] > 1.0f) outColor[1] = 1.0f;

    outColor[2] = GammaCorrectColorChannel(outColor[2]);
    if (outColor[2] < 0.0f) outColor[2] = 0.0f;
    if (outColor[2] > 1.0f) outColor[2] = 1.0f;

    return 1;
}

/*
================
CalculateGroundLightingForAllStaticModels

Process all ground-lit models.
Walks g_groundLitList, computes lighting, formats as hex "RRGGBBAA" entity key.
================
*/
void CalculateGroundLightingForAllStaticModels(void)
{
    GroundLitModel_t *node;
    float color[3];
    float intensity;
    char buf[16];

    node = g_groundLitList;
    if (!node)
        return;

    do
    {
        if (CalculateGroundLighting_Worker(node->pos, node->radius, node->dir,
                                            color, &intensity))
        {
            unsigned char r, g, b, a;

            r = EncodeFloatInByte(color[0]);
            g = EncodeFloatInByte(color[1]);
            b = EncodeFloatInByte(color[2]);
            a = EncodeFloatInByte(intensity);

            Com_sprintf(buf, 16, "%02x%02x%02x%02x", r, g, b, a);

            SetKeyValue(node->entity, "gndLt", buf);
        }

        {
            GroundLitModel_t *next = node->next;
            g_groundLitList = next;
            Z_FreeInternal(node);
            node = g_groundLitList;
        }
    } while (node);
}

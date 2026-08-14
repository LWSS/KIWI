/*
 * lighting_types.h — Struct definitions for the lighting system.
 *
 * Shared between groundlight.c, lighting.c, and other lighting-related files.
 * All offsets verified against cod2rad64 LST.
 */

#ifndef LIGHTING_TYPES_H
#define LIGHTING_TYPES_H

/*
 * SampleVars — per-sample lighting variable data, 96 bytes (0x60).
 * Allocated from g_sampleVarsPool in blocks of 96 bytes (3 * 32).
 * Size confirmed: malloc(usefulSampleCount * 0x60) in Lighting_InitSamples.
 *
 * Known field accesses from LST:
 *   +0x00..+0x0F: intensity data (4 floats, accumulated in groundlight.c inner loop)
 *   +0x40..+0x4B: gathered light RGB (3 floats, read by Lighting_GetGatheredLight)
 *
 * Full layout partially known — remaining fields used by lighting subsystem.
 */
typedef struct SampleVars
{
    float intensity[4];     /* +0x00: intensity accumulation data */
    float field_10[4];      /* +0x10: unknown lighting data */
    float field_20[4];      /* +0x20: unknown lighting data */
    float field_30[4];      /* +0x30: unknown lighting data */
    float gathered[3];      /* +0x40: gathered light RGB */
    float field_4C;         /* +0x4C: unknown */
    float field_50[4];      /* +0x50: unknown lighting data */
} SampleVars;               /* total: 0x60 (96 bytes) */

/*
 * LightingSample — lighting sample reference, pointed to by LightingHit.
 * +0x00: vars pointer (SampleVars*)
 */
typedef struct LightingSample
{
    SampleVars *vars;   /* +0x00: lighting variable data (96 bytes) */
} LightingSample;

/*
 * LightmapSample — 32-byte sample slot in the lightmap data buffer.
 * Accessed by GetLightingSample and BuildFinalLightmap_PerPixel.
 * g_lightingSamples is an array of these, indexed by
 * ((lmapIndex * 512 + t) * 512 + s).
 */
typedef struct LightmapSample
{
    SampleVars *vars;       /* +0x00: pointer to allocated vars (or NULL) */
    float weight;           /* +0x08: sample weight (0 = unused) */
    unsigned char pad0C[20];/* +0x0C: padding to 32 bytes */
} LightmapSample;          /* total: 0x20 (32 bytes) */

/*
 * LightingHit — hit result from FindLightingSamplesAndNormal, 16 bytes (0x10).
 * Stride confirmed from LST (add rdi, 10h).
 */
typedef struct LightingHit
{
    LightingSample *sample; /* +0x00: pointer to lighting sample */
    float weight;           /* +0x08: hit weight */
    int pad0C;              /* +0x0C: padding */
} LightingHit;              /* total: 0x10 (16 bytes) */

#endif /* LIGHTING_TYPES_H */

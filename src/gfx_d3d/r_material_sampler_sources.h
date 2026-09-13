#pragma once
// Shared raw-loader/compiler sampler mappings and update frequencies.
const CodeSamplerSource s_lightmapSamplers[3] =
{
    { "primary", TEXTURE_SRC_CODE_LIGHTMAP_PRIMARY, NULL, 0, 0 },
    { "secondary", TEXTURE_SRC_CODE_LIGHTMAP_SECONDARY, NULL, 0, 0 },
    {0}
};

const CodeSamplerSource s_lightSamplers[2] =
{
    { "attenuation", TEXTURE_SRC_CODE_LIGHT_ATTENUATION, NULL, 0, 0 },
    {0}
};

#ifdef KISAK_RADIANT
// One extra entry vs SP/MP: the editor's case_texture.tech binds sampler.caseTexture
// (TEXTURE_SRC_CODE_CASE_TEXTURE). Without this entry Material_DefaultSamplerSourceFromTable
// can't resolve the name -> Material_RegisterTechnique('case_texture') returns NULL -> the
// whole l_sm_* world techset aborts -> world materials fall to "2d". The image is bound
// per-surface at draw time by R_GetCaseTexture (r_shade.cpp), not from a static code image.
const CodeSamplerSource s_codeSamplers[21] =
#else
const CodeSamplerSource s_codeSamplers[20] =
#endif
{
  { "white", TEXTURE_SRC_CODE_WHITE, NULL, 0, 0 },
  { "black", TEXTURE_SRC_CODE_BLACK, NULL, 0, 0 },
  { "identityNormalMap", TEXTURE_SRC_CODE_IDENTITY_NORMAL_MAP, NULL, 0, 0 },
  { "lightmap", TEXTURE_SRC_CODE_LIGHTMAP_PRIMARY, s_lightmapSamplers, 0, 0 },
  { "outdoor", TEXTURE_SRC_CODE_OUTDOOR, NULL, 0, 0 },
  { "shadowmapSun", TEXTURE_SRC_CODE_SHADOWMAP_SUN, NULL, 0, 0 },
  { "shadowmapSpot", TEXTURE_SRC_CODE_SHADOWMAP_SPOT, NULL, 0, 0 },
  { "shadowCookie", TEXTURE_SRC_CODE_SHADOWCOOKIE, NULL, 0, 0 },
  { "dynamicShadow", TEXTURE_SRC_CODE_DYNAMIC_SHADOWS, NULL, 0, 0 },
  { "feedback", TEXTURE_SRC_CODE_FEEDBACK, NULL, 0, 0 },
  { "resolvedPostSun", TEXTURE_SRC_CODE_RESOLVED_POST_SUN, NULL, 0, 0 },
  { "resolvedScene", TEXTURE_SRC_CODE_RESOLVED_SCENE, NULL, 0, 0 },
  { "postEffect0", TEXTURE_SRC_CODE_POST_EFFECT_0, NULL, 0, 0 },
  { "postEffect1", TEXTURE_SRC_CODE_POST_EFFECT_1, NULL, 0, 0 },
  { "sky", TEXTURE_SRC_CODE_SKY, NULL, 0, 0 },
  { "light", TEXTURE_SRC_CODE_LIGHT_ATTENUATION, s_lightSamplers, 0, 0 },
  { "floatZ", TEXTURE_SRC_CODE_FLOATZ, NULL, 0, 0 },
  { "processedFloatZ", TEXTURE_SRC_CODE_PROCESSED_FLOATZ, NULL, 0, 0 },
  { "rawFloatZ", TEXTURE_SRC_CODE_RAW_FLOATZ, NULL, 0, 0 },
#ifdef KISAK_RADIANT
  { "caseTexture", TEXTURE_SRC_CODE_CASE_TEXTURE, NULL, 0, 0 },  // editor only (idb 0x6341cc)
#endif
  { NULL, TEXTURE_SRC_CODE_BLACK, NULL, 0, 0 }
}; // idb

const MaterialUpdateFrequency s_codeSamplerUpdateFreq[27] =
{
  MTL_UPDATE_RARELY,
  MTL_UPDATE_RARELY,
  MTL_UPDATE_RARELY,
  MTL_UPDATE_RARELY,
  MTL_UPDATE_CUSTOM,
  MTL_UPDATE_CUSTOM,
  MTL_UPDATE_RARELY,
  MTL_UPDATE_RARELY,
  MTL_UPDATE_RARELY,
  MTL_UPDATE_PER_OBJECT,
  MTL_UPDATE_RARELY,
  MTL_UPDATE_RARELY,
  MTL_UPDATE_RARELY,
  MTL_UPDATE_RARELY,
  MTL_UPDATE_RARELY,
  MTL_UPDATE_PER_OBJECT,
  MTL_UPDATE_PER_OBJECT,
  MTL_UPDATE_RARELY,
  MTL_UPDATE_RARELY,
  MTL_UPDATE_RARELY,
  MTL_UPDATE_RARELY,
  MTL_UPDATE_PER_OBJECT,
  MTL_UPDATE_PER_OBJECT,
  MTL_UPDATE_PER_OBJECT,
  MTL_UPDATE_PER_OBJECT,
  MTL_UPDATE_PER_OBJECT,
  MTL_UPDATE_CUSTOM
}; // idb

const CodeSamplerSource s_defaultCodeSamplers[18] =
{
  { "shadowmapSamplerSun", TEXTURE_SRC_CODE_SHADOWMAP_SUN, NULL, 0, 0 },
  { "shadowmapSamplerSpot", TEXTURE_SRC_CODE_SHADOWMAP_SPOT, NULL, 0, 0 },
  { "shadowCookieSampler", TEXTURE_SRC_CODE_SHADOWCOOKIE, NULL, 0, 0 },
  { "feedbackSampler", TEXTURE_SRC_CODE_FEEDBACK, NULL, 0, 0 },
  { "dynamicShadowSampler", TEXTURE_SRC_CODE_DYNAMIC_SHADOWS, NULL, 0, 0 },
  { "floatZSampler", TEXTURE_SRC_CODE_FLOATZ, NULL, 0, 0 },
  { "processedFloatZSampler", TEXTURE_SRC_CODE_PROCESSED_FLOATZ, NULL, 0, 0 },
  { "rawFloatZSampler", TEXTURE_SRC_CODE_RAW_FLOATZ, NULL, 0, 0 },
  { "attenuationSampler", TEXTURE_SRC_CODE_LIGHT_ATTENUATION, NULL, 0, 0 },
  { "lightmapSamplerPrimary", TEXTURE_SRC_CODE_LIGHTMAP_PRIMARY, NULL, 0, 0 },
  {
    "lightmapSamplerSecondary",
    TEXTURE_SRC_CODE_LIGHTMAP_SECONDARY,
    NULL,
    0,
    0
  },
  { "modelLightingSampler", TEXTURE_SRC_CODE_MODEL_LIGHTING, NULL, 0, 0 },
  { "cinematicYSampler", TEXTURE_SRC_CODE_CINEMATIC_Y, NULL, 0, 0 },
  { "cinematicCrSampler", TEXTURE_SRC_CODE_CINEMATIC_CR, NULL, 0, 0 },
  { "cinematicCbSampler", TEXTURE_SRC_CODE_CINEMATIC_CB, NULL, 0, 0 },
  { "cinematicASampler", TEXTURE_SRC_CODE_CINEMATIC_A, NULL, 0, 0 },
  { "reflectionProbeSampler", TEXTURE_SRC_CODE_REFLECTION_PROBE, NULL, 0, 0 },
  { NULL, TEXTURE_SRC_CODE_BLACK, NULL, 0, 0 }
}; // idb


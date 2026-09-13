#pragma once
// Shared raw-loader/compiler constant names and runtime indices.
const CodeConstantSource s_sunConsts[4] = 
{
    { "position", 0x23, 0, 0, 0 },
    { "diffuse", 0x24, 0, 0, 0 },
    { "specular", 0x25, 0, 0, 0 },
    {0}
};

const CodeConstantSource s_lightConsts[7] =
{
    { "position", 0, 0, 0, 0 },
    { "diffuse", 1, 0, 0, 0 },
    { "specular", 2, 0, 0, 0 },
    { "spotDir", 3, 0, 0, 0 },
    { "spotFactors", 4, 0, 0, 0 },
    { "falloffPlacement", 0xB, 0, 0, 0 },
    {0}
};

const CodeConstantSource s_nearPlaneConsts[4] =
{
    { "org", 5, 0, 0, 0 },
    { "dx", 6, 0, 0, 0 },
    { "dy", 7, 0, 0, 0 }
};

const CodeConstantSource s_defaultCodeConsts[14] =
{
    { "nearPlaneOrg", 5, 0, 0, 0 },
    { "nearPlaneDx", 6, 0, 0, 0 },
    { "nearPlaneDy", 7, 0, 0, 0 },
    { "sunPosition", 0x23, 0, 0, 0 },
    { "sunDiffuse", 0x24, 0, 0, 0 },
    { "sunSpecular", 0x25, 0, 0, 0 },
    { "lightPosition", 0, 0, 0, 0 },
    { "lightDiffuse", 1, 0, 0, 0 },
    { "lightSpecular", 2, 0, 0, 0 },
    { "lightSpotDir", 3, 0, 0, 0 },
    { "lightSpotFactors", 4, 0, 0, 0 },
    { "lightFalloffPlacement", 0xB, 0, 0, 0 },
    { "spotShadowmapPixelAdjust", 0x32, 0, 0, 0 },
    {0}
};

const CodeConstantSource s_codeConsts[73] =
{
  { "nearPlane", 91u, s_nearPlaneConsts, 0, 0 },
  { "sun", 91u, s_sunConsts, 0, 0 },
  { "light", 91u, s_lightConsts, 0, 0 },
  { "baseLightingCoords", 57u, NULL, 0, 0 },
  { "lightingLookupScale", 38u, NULL, 0, 0 },
  { "debugBumpmap", 39u, NULL, 0, 0 },
  { "pixelCostFracs", 19u, NULL, 0, 0 },
  { "pixelCostDecode", 20u, NULL, 0, 0 },
  { "materialColor", 40u, NULL, 0, 0 },
  { "fogConsts", 41u, NULL, 0, 0 },
  { "fogColor", 42u, NULL, 0, 0 },
  { "glowSetup", 43u, NULL, 0, 0 },
  { "glowApply", 44u, NULL, 0, 0 },
  { "filterTap", 21u, NULL, 8, 1 },
  { "codeMeshArg", 55u, NULL, 2, 1 },
  { "renderTargetSize", 10u, NULL, 0, 0 },
  { "shadowmapSwitchPartition", 32u, NULL, 0, 0 },
  { "shadowmapScale", 33u, NULL, 0, 0 },
  { "shadowmapPolygonOffset", 9u, NULL, 0, 0 },
  { "shadowParms", 8u, NULL, 0, 0 },
  { "zNear", 34u, NULL, 0, 0 },
  { "clipSpaceLookupScale", 51u, NULL, 0, 0 },
  { "clipSpaceLookupOffset", 52u, NULL, 0, 0 },
  { "dofEquationViewModelAndFarBlur", 12u, NULL, 0, 0 },
  { "dofEquationScene", 13u, NULL, 0, 0 },
  { "dofLerpScale", 14u, NULL, 0, 0 },
  { "dofLerpBias", 15u, NULL, 0, 0 },
  { "dofRowDelta", 16u, NULL, 0, 0 },
  { "depthFromClip", 54u, NULL, 0, 0 },
  { "outdoorFeatherParms", 48u, NULL, 0, 0 },
  { "envMapParms", 49u, NULL, 0, 0 },
  { "colorMatrixR", 29u, NULL, 0, 0 },
  { "colorMatrixG", 30u, NULL, 0, 0 },
  { "colorMatrixB", 31u, NULL, 0, 0 },
  { "colorBias", 45u, NULL, 0, 0 },
  { "colorTintBase", 46u, NULL, 0, 0 },
  { "colorTintDelta", 47u, NULL, 0, 0 },
  { "gameTime", 18u, NULL, 0, 0 },
  { "particleCloudColor", 17u, NULL, 0, 0 },
  { "particleCloudMatrix", 53u, NULL, 0, 0 },
  { "worldMatrix", 58u, NULL, 0, 0 },
  { "inverseWorldMatrix", 59u, NULL, 0, 0 },
  { "transposeWorldMatrix", 60u, NULL, 0, 0 },
  { "inverseTransposeWorldMatrix", 61u, NULL, 0, 0 },
  { "viewMatrix", 62u, NULL, 0, 0 },
  { "inverseViewMatrix", 63u, NULL, 0, 0 },
  { "transposeViewMatrix", 64u, NULL, 0, 0 },
  { "inverseTransposeViewMatrix", 65u, NULL, 0, 0 },
  { "projectionMatrix", 66u, NULL, 0, 0 },
  { "inverseProjectionMatrix", 67u, NULL, 0, 0 },
  { "transposeProjectionMatrix", 68u, NULL, 0, 0 },
  { "inverseTransposeProjectionMatrix", 69u, NULL, 0, 0 },
  { "worldViewMatrix", 70u, NULL, 0, 0 },
  { "inverseWorldViewMatrix", 71u, NULL, 0, 0 },
  { "transposeWorldViewMatrix", 72u, NULL, 0, 0 },
  { "inverseTransposeWorldViewMatrix", 73u, NULL, 0, 0 },
  { "viewProjectionMatrix", 74u, NULL, 0, 0 },
  { "inverseViewProjectionMatrix", 75u, NULL, 0, 0 },
  { "transposeViewProjectionMatrix", 76u, NULL, 0, 0 },
  { "inverseTransposeViewProjectionMatrix", 77u, NULL, 0, 0 },
  { "worldViewProjectionMatrix", 78u, NULL, 0, 0 },
  { "inverseWorldViewProjectionMatrix", 79u, NULL, 0, 0 },
  { "transposeWorldViewProjectionMatrix", 80u, NULL, 0, 0 },
  { "inverseTransposeWorldViewProjectionMatrix", 81u, NULL, 0, 0 },
  { "shadowLookupMatrix", 82u, NULL, 0, 0 },
  { "inverseShadowLookupMatrix", 83u, NULL, 0, 0 },
  { "transposeShadowLookupMatrix", 84u, NULL, 0, 0 },
  { "inverseTransposeShadowLookupMatrix", 85u, NULL, 0, 0 },
  { "worldOutdoorLookupMatrix", 86u, NULL, 0, 0 },
  { "inverseWorldOutdoorLookupMatrix", 87u, NULL, 0, 0 },
  { "transposeWorldOutdoorLookupMatrix", 88u, NULL, 0, 0 },
  { "inverseTransposeWorldOutdoorLookupMatrix", 89u, NULL, 0, 0 },
  { NULL, 0u, NULL, 0, 0 }
}; // idb


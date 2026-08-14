/*
texturevecs.c — Texture vector computation

Reconstructed from cod2map.exe by Rose.
*/

#include "cod4map.h"

int g_textureBasisVecs[NUM_TEXTURE_AXES * TEXTURE_BASIS_STRIDE] = {
    /* normal        S axis          T axis */
    /* +Z ceiling */ 0x00000000, 0x00000000, 0x3F800000, 0x3F800000, 0x00000000, 0x00000000, 0x00000000, 0xBF800000, 0x00000000,
    /* -Z floor   */ 0x00000000, 0x00000000, 0xBF800000, 0x3F800000, 0x00000000, 0x00000000, 0x00000000, 0xBF800000, 0x00000000,
    /* +X wall    */ 0x3F800000, 0x00000000, 0x00000000, 0x00000000, 0x3F800000, 0x00000000, 0x00000000, 0x00000000, 0xBF800000,
    /* -X wall    */ 0xBF800000, 0x00000000, 0x00000000, 0x00000000, 0x3F800000, 0x00000000, 0x00000000, 0x00000000, 0xBF800000,
    /* +Y wall    */ 0x00000000, 0x3F800000, 0x00000000, 0x3F800000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xBF800000,
    /* -Y wall    */ 0x00000000, 0xBF800000, 0x00000000, 0x3F800000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xBF800000
};

char s_assertDisable_btvPvec0R;
char s_assertDisable_btvPvec0S;
char s_assertDisable_btvPvec0T;
char s_assertDisable_btvPvec1R;
char s_assertDisable_btvPvec1S;
char s_assertDisable_btvPvec1T;
char s_assertDisable_btvSvEqTv;
char s_assertDisable_tvNormal;
char s_assertDisable_tvS;
char s_assertDisable_tvT;
char s_assertDisable_tvXv;
char s_assertDisable_tvYv;


/*
================
TexturePlanesFromBasis

Picks the best texture projection axis from 6 cardinal
directions based on the surface normal.
================
*/
int TexturePlanesFromBasis(float *normal, int *xAxis, int *yAxis)
{
  float bestDot;
  int bestAxis, i;
  
  /* find the cardinal direction most aligned with the normal */
  bestDot = 0.0f;
  bestAxis = 0;
  for ( i = 0; i < NUM_TEXTURE_AXES; i++ )
  {
    float dot = DotProduct(normal, (float *)&g_textureBasisVecs[TEXTURE_BASIS_STRIDE * i]);
    if ( dot > bestDot )
    {
      bestDot = dot;
      bestAxis = i;
    }
  }

  /* copy texture basis vectors for the chosen axis */
  xAxis[0] = g_textureBasisVecs[TEXTURE_BASIS_STRIDE * bestAxis + 3];
  xAxis[1] = g_textureBasisVecs[TEXTURE_BASIS_STRIDE * bestAxis + 4];
  xAxis[2] = g_textureBasisVecs[TEXTURE_BASIS_STRIDE * bestAxis + 5];
  yAxis[0] = g_textureBasisVecs[TEXTURE_BASIS_STRIDE * bestAxis + 6];
  yAxis[1] = g_textureBasisVecs[TEXTURE_BASIS_STRIDE * bestAxis + 7];
  yAxis[2] = g_textureBasisVecs[TEXTURE_BASIS_STRIDE * bestAxis + 8];

  return 0;
}

/*
================
TextureVecsForNormal

Computes texture basis axes for a surface normal and
identifies the active component indices (s, t).
================
*/
void TextureVecsForNormal(float *normal, float *xv, float *yv, int *s, int *t)
{
  Assert(normal, s_assertDisable_tvNormal);
  Assert(xv, s_assertDisable_tvXv);
  Assert(yv, s_assertDisable_tvYv);
  Assert(s, s_assertDisable_tvS);
  Assert(t, s_assertDisable_tvT);
  TexturePlanesFromBasis(normal, (int *)xv, (int *)yv);
  if ( xv[0] == 0.0 )
  {
    if ( xv[1] == 0.0 )
      *s = 2;
    else
      *s = 1;
  }
  else
  {
    *s = 0;
  }
  if ( yv[0] == 0.0 )
  {
    if ( yv[1] == 0.0 )
      *t = 2;
    else
      *t = 1;
  }
  else
  {
    *t = 0;
  }
}

/*
================
BuildTextureVecs

Builds texture mapping vectors from a surface normal,
scale, shift, rotation and skew parameters.
================
*/
float *BuildTextureVecs(float *normal, float *scale, float *shift, float rotation, float skew, float *texVecs)
{
  float scaleX, scaleY_stored;
  int sv, tv, rv;
  int svIdx, tvIdx;
  float sAxisVal, tAxisVal;
  double invScaleX, invScaleY;
  double scaledS, scaledT;
  float xVec[3], yVec[3];

  scaleX = scale[0];
  scaleY_stored = scale[1];
  if ( scaleX == 0.0 )
    scaleX = 128.0f;
  if ( scaleY_stored == 0.0 )
    scaleY_stored = 128.0f;

  TextureVecsForNormal(normal, xVec, yVec, &svIdx, &tvIdx);
  sv = svIdx;
  tv = tvIdx;
  Assert(svIdx != tvIdx, s_assertDisable_btvSvEqTv);
  sAxisVal = xVec[sv];
  rv = 3 - sv - tv;
  Assert(sAxisVal == 1.0 || sAxisVal == -1.0, s_assertDisable_btvPvec0S);
  Assert(xVec[tv] == 0.0, s_assertDisable_btvPvec0T);
  Assert(xVec[rv] == 0.0, s_assertDisable_btvPvec0R);
  Assert(yVec[sv] == 0.0, s_assertDisable_btvPvec1S);
  tAxisVal = yVec[tv];
  Assert(tAxisVal == 1.0 || tAxisVal == -1.0, s_assertDisable_btvPvec1T);
  Assert(yVec[rv] == 0.0, s_assertDisable_btvPvec1R);
  
  /* compute rotated and scaled texture vectors */
  float sinVal, cosVal;
  SinCos(rotation, &sinVal, &cosVal);

  invScaleX = 1.0 / scaleX;
  invScaleY = 1.0 / scaleY_stored;
  scaledS = invScaleX * xVec[sv];
  scaledT = invScaleY * yVec[tv];

  texVecs[sv] = cosVal * scaledS;
  texVecs[tv] = scaledS * sinVal;

  texVecs[rv] = 0.0f;
  texVecs[sv + 4] = -(sinVal * scaledT);
  texVecs[tv + 4] = scaledT * cosVal;
  texVecs[rv + 4] = 0.0f;

  /* apply skew to S vector — use double intermediates to match x87 _PC_53 precision */
  texVecs[0] = (float)((double)texVecs[0] + (double)skew * (double)texVecs[4]);
  texVecs[1] = (float)((double)texVecs[1] + (double)skew * (double)texVecs[5]);
  texVecs[2] = (float)((double)texVecs[2] + (double)skew * (double)texVecs[6]);

  /* shift */
  texVecs[3] = -shift[0] / scaleX;
  texVecs[7] = -shift[1] / scaleY_stored;

  return texVecs;
}

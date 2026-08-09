/*
tangentspace.c — Tangent space generation

Reconstructed from cod2map.exe by Rose.
*/

#include "cod4map.h"

char s_assertDisable_tsI0;
char s_assertDisable_tsI1;
char s_assertDisable_tsI2;
char s_assertDisable_tsIndices;
char s_assertDisable_tsSources;
char s_assertDisable_tsVertCount;


/*
================
TangentSpaceCalcTBForTriangle

Computes tangent and bitangent vectors for a triangle from positions and UVs.
================
*/
void TangentSpaceCalcTBForTriangle(float *pos0, float *pos1, float *pos2, float *uv0, float *uv1, float *uv2, float *tangent, float *bitangent)
{
  double du1, du2;
  double dx1, dx2, dy1, dy2, dz1, dz2;
  
  /* dv1 is float (32-bit) and dv2_80 is double (80-bit FPU) — precision difference
     is intentional and required for bit-identical output with the original exe */
  float dv1;
  double dv2_80;
  float dv2;

  du1 = (double)uv1[0] - (double)uv0[0];
  du2 = (double)uv2[0] - (double)uv0[0];
  dv1 = uv1[1] - uv0[1];
  dv2_80 = (double)uv2[1] - (double)uv0[1];
  dv2 = (float)dv2_80;

  dx1 = (double)pos1[0] - (double)pos0[0];
  dx2 = (double)pos2[0] - (double)pos0[0];
  dy1 = (double)pos1[1] - (double)pos0[1];
  dy2 = (double)pos2[1] - (double)pos0[1];
  dz1 = (double)pos1[2] - (double)pos0[2];
  dz2 = (double)pos2[2] - (double)pos0[2];

  /* choose winding order based on UV cross product sign */
  if ( Det2x2(dv1, du2, dv2_80, du1) <= 0.0 )
  {
    tangent[0]   = Det2x2(dx1, dv2, dx2, dv1);
    tangent[1]   = Det2x2(dy1, dv2, dy2, dv1);
    tangent[2]   = Det2x2(dz1, dv2, dz2, dv1);
    bitangent[0] = Det2x2(dx2, du1, dx1, du2);
    bitangent[1] = Det2x2(dy2, du1, dy1, du2);
    bitangent[2] = Det2x2(dz2, du1, dz1, du2);
  }
  else
  {
    tangent[0]   = Det2x2(dv1, dx2, dx1, dv2);
    tangent[1]   = Det2x2(dv1, dy2, dy1, dv2);
    tangent[2]   = Det2x2(dv1, dz2, dz1, dv2);
    bitangent[0] = Det2x2(dx1, du2, dx2, du1);
    bitangent[1] = Det2x2(dy1, du2, dy2, du1);
    bitangent[2] = Det2x2(dz1, du2, dz2, du1);
  }

  VecNormalize(tangent);
  VecNormalize(bitangent);
}

/*
================
TangentSpaceAngleBetweenVectors

Computes the angle between two normalized vectors.
================
*/
double TangentSpaceAngleBetweenVectors(float *vecA, float *vecB)
{
  double dot;
  float dotClamped;

  /* x87 eval order 2,1,0 */
  dot = DotProduct210(vecB, vecA);
  dotClamped = dot;

  if ( dot <= -1.0 )
    return -Q_PI;
  if ( dotClamped < 1.0f )
    return acos(dotClamped);
  return Q_PI;
}


#define MAX_TANGENT_SPACE_VERTS 0x10000
#define TS_POS(src, idx)   ((float *)((src)->pos + (idx) * (src)->posStride))
#define TS_NORM(src, idx)  ((float *)((src)->normal + (idx) * (src)->normalStride))
#define TS_UV(src, idx)    ((float *)((src)->uv + (idx) * (src)->uvStride))
#define TS_TAN(src, idx)   ((float *)((src)->tangent + (idx) * (src)->tangentStride))
#define TS_BITAN(src, idx) ((float *)((src)->bitangent + (idx) * (src)->bitangentStride))

/*
================
TangentSpaceCalcTriangleAngles

Computes the three interior angles of a triangle.
================
*/
void TangentSpaceCalcTriangleAngles(TangentSources_t *sources, float *angles, int idx0, int idx1, int idx2)
{
  float *v0 = TS_POS(sources, idx0);
  float *v1 = TS_POS(sources, idx1);
  float *v2 = TS_POS(sources, idx2);
  vec3_t edge01, edge12, edge20;

  VectorSubtract(v0, v1, edge01);
  VectorSubtract(v1, v2, edge12);
  VectorSubtract(v2, v0, edge20);
  VecNormalize(edge01);
  VecNormalize(edge12);
  VecNormalize(edge20);

  angles[0] = TangentSpaceAngleBetweenVectors(edge01, edge20);
  angles[1] = TangentSpaceAngleBetweenVectors(edge12, edge01);
  angles[2] = TangentSpaceAngleBetweenVectors(edge20, edge12);
}

/*
================
TangentSpaceGenerate

Generates tangent space vectors for mesh vertices.
Zeroes output arrays, accumulates angle-weighted tangent/bitangent
per triangle, then orthogonalizes per vertex via Gram-Schmidt.
================
*/
float *TangentSpaceGenerate(TangentSources_t *sources, int vertCount, unsigned short *indices, int indexCount)
{
  int i;
  double dotNT;
  
  /* volatile: forces 32-bit float precision per iteration, prevents FPU register hoisting */
  volatile float tangentTemp[3];
  volatile float bitangentTemp[3];
  float triAngles[3];

  Assert(sources, s_assertDisable_tsSources);
  Assert(indices, s_assertDisable_tsIndices);
  Assert(vertCount <= MAX_TANGENT_SPACE_VERTS, s_assertDisable_tsVertCount);

  /* phase 1: zero all tangent and bitangent output vectors */
  for ( i = 0; i < vertCount; i++ )
  {
    VectorClear(TS_TAN(sources, i));
    VectorClear(TS_BITAN(sources, i));
  }

  /* phase 2: accumulate angle-weighted tangent/bitangent per triangle */
  if ( indexCount > 0 )
  {
    unsigned short *idxBase = indices;

    for ( i = 0; i < indexCount; i += 3 )
    {
      int i0 = idxBase[i];
      int i1 = idxBase[i + 1];
      int i2 = idxBase[i + 2];
      float *tan, *bitan;
      int j;

      Assert(i0 < vertCount, s_assertDisable_tsI0);
      Assert(i1 < vertCount, s_assertDisable_tsI1);
      Assert(i2 < vertCount, s_assertDisable_tsI2);

      TangentSpaceCalcTBForTriangle(
        TS_POS(sources, i0), TS_POS(sources, i1), TS_POS(sources, i2),
        TS_UV(sources, i0), TS_UV(sources, i1), TS_UV(sources, i2),
        (float *)tangentTemp, (float *)bitangentTemp);
      TangentSpaceCalcTriangleAngles(sources, triAngles, i0, i1, i2);

      /* accumulate angle-weighted T and B for each vertex of this triangle */
      { int triVerts[3] = { i0, i1, i2 };
      for ( j = 0; j < 3; j++ )
      {
        tan = TS_TAN(sources, triVerts[j]);
        tan[0] = FMA1(tan[0], tangentTemp[0], triAngles[j]);
        tan[1] = FMA1(tan[1], tangentTemp[1], triAngles[j]);
        tan[2] = FMA1(tan[2], tangentTemp[2], triAngles[j]);

        bitan = TS_BITAN(sources, triVerts[j]);
        bitan[0] = FMA1(bitan[0], bitangentTemp[0], triAngles[j]);
        bitan[1] = FMA1(bitan[1], bitangentTemp[1], triAngles[j]);
        bitan[2] = FMA1(bitan[2], bitangentTemp[2], triAngles[j]);
      } }
    }
  }

  /* phase 3: per-vertex Gram-Schmidt orthogonalization */
  for ( i = 0; i < vertCount; i++ )
  {
    float *n = TS_NORM(sources, i);
    float *t = TS_TAN(sources, i);
    float *b = TS_BITAN(sources, i);

    /* orthogonalize tangent against normal: t = t - n * dot(n, t) */
    /* x87 eval order 1,2,0 */
    dotNT = -DotProduct120(n, t);
    t[0] = FMA1(t[0], dotNT, n[0]);
    t[1] = FMA1(t[1], dotNT, n[1]);
    t[2] = FMA1(t[2], dotNT, n[2]);

    /* if tangent degenerates, derive from bitangent or normal */
    if ( VecNormalize(t) < MIN_NORMAL_LENGTH )
    {
      CrossProduct(b, n, t);
      if ( VecNormalize(t) < MIN_NORMAL_LENGTH )
        PerpendicularVector(n, t);
    }

    /* compute bitangent from cross product, flip if needed */
    CrossProduct(n, t, (float *)bitangentTemp);
    /* x87 eval order 2,1,0 */
    if ( DotProduct210(bitangentTemp, b) >= 0.0 )
      VectorCopy(bitangentTemp, b);
    else
      VectorScale(bitangentTemp, -1, b);
  }

  return TS_NORM(sources, 0);
}

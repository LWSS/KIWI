/*
mesh.c — Mesh and curve subdivision

Reconstructed from cod2map.exe by Rose.
*/

#include "cod4map.h"

MeshVert_t expand[MAX_EXPANDED_AXIS][MAX_EXPANDED_AXIS];


/*
================
LerpDrawVert

50/50 interpolation between two draw verts
================
*/
void LerpDrawVert(MeshVert_t *a, MeshVert_t *b, MeshVert_t *out)
{
  unsigned char *ca, *cb, *co;

  /* interpolate position */
  out->pos[0] = MIDF(a->pos[0], b->pos[0]);
  out->pos[1] = MIDF(a->pos[1], b->pos[1]);
  out->pos[2] = MIDF(a->pos[2], b->pos[2]);

  /* interpolate texture coordinates */
  out->st[0] = MIDF(a->st[0], b->st[0]);
  out->st[1] = MIDF(a->st[1], b->st[1]);

  /* interpolate lightmap coordinates */
  out->lmSt[0] = MIDF(a->lmSt[0], b->lmSt[0]);
  out->lmSt[1] = MIDF(a->lmSt[1], b->lmSt[1]);

  /* average packed RGBA color bytes */
  ca = (unsigned char *)&a->color;
  cb = (unsigned char *)&b->color;
  co = (unsigned char *)&out->color;
  co[0] = (ca[0] + cb[0]) >> 1;
  co[1] = (ca[1] + cb[1]) >> 1;
  co[2] = (ca[2] + cb[2]) >> 1;
  co[3] = (ca[3] + cb[3]) >> 1;
}

/*
================
FreeMesh

Frees a mesh allocation
================
*/
void FreeMesh(void *mesh)
{
  free(mesh);
}

/*
================
CopyMesh

Deep-copies a mesh (width, height, vertex data)
================
*/
Mesh_t *CopyMesh(Mesh_t *src)
{
  int vertSize;
  Mesh_t *out;

  /* allocate header + vertex data in one block */
  vertSize = sizeof(MeshVert_t) * src->width * src->height;
  out = malloc(sizeof(Mesh_t) + vertSize);
  out->width = src->width;
  out->height = src->height;
  out->verts = (MeshVert_t *)(out + 1);
  memcpy(out->verts, src->verts, vertSize);
  return out;
}

/*
================
PutMeshOnCurve

Drops approximating points onto the quadratic B-spline curve.
Odd-indexed rows/columns are control points that approximate the curve —
this function projects them onto the actual curve by averaging with their
================
*/
int PutMeshOnCurve(int width, int height, int a3, MeshVert_t *verts)
{
  MeshVert_t *cur;
  MeshVert_t t1, t2;
  int col, row, k, oddRows, oddCols;

  oddRows = (height - 1) / 2;
  oddCols = (width - 1) / 2;

  /* project control points onto curve along rows (lerp odd rows toward even neighbors) */
  for ( col = 0; col < width; col++ )
  {
    for ( k = 0; k < oddRows; k++ )
    {
      cur = &verts[(1 + 2 * k) * width + col];
      LerpDrawVert(cur, cur + width, &t2);
      LerpDrawVert(cur, cur - width, &t1);
      LerpDrawVert(&t2, &t1, cur);
    }
  }

  /* project control points onto curve along columns (lerp odd cols toward even neighbors) */
  for ( row = 0; row < height; row++ )
  {
    for ( k = 0; k < oddCols; k++ )
    {
      cur = &verts[row * width + 1 + 2 * k];
      LerpDrawVert(cur, cur + 1, &t2);
      LerpDrawVert(cur, cur - 1, &t1);
      LerpDrawVert(&t2, &t1, cur);
    }
  }

  return width;
}

/*
================
RemoveLinearMeshColumnsRows

Removes columns and rows from a mesh that are just linear
interpolations of their neighbors, within a tolerance of 0.1 units.
================
*/
Mesh_t *RemoveLinearMeshColumnsRows(Mesh_t *inMesh, int *widthTable, int *heightTable)
{
  int i, j, k;
  float len, maxLength;
  vec3_t proj, dir;
  int width, height;
  unsigned int totalBytes;
  Mesh_t *out;

  width = inMesh->width;
  height = inMesh->height;

  /* copy input mesh verts into the expand grid */
  for ( i = 0; i < inMesh->width; i++ )
  {
    for ( j = 0; j < inMesh->height; j++ )
    {
      expand[j][i] = inMesh->verts[j * inMesh->width + i];
    }
  }

  /* remove columns that are linear interpolations */
  for ( j = 1; j < width - 1; j++ )
  {
    maxLength = 0;
    for ( i = 0; i < height; i++ )
    {
      ProjectPointOntoVector(expand[i][j].pos, expand[i][j - 1].pos, expand[i][j + 1].pos, proj);
      VectorSubtract(expand[i][j].pos, proj, dir);
      len = VectorLength(dir);
      if ( len > maxLength )
        maxLength = len;
    }

    if ( maxLength < MESH_MIN_EDGE_LENGTH )
    {
      width--;
      for ( i = 0; i < height; i++ )
      {
        for ( k = j; k < width; k++ )
        {
          expand[i][k] = expand[i][k + 1];
        }
      }

      /* shift width table entries if provided */
      if ( widthTable )
      {
        for ( k = j; k < width; k++ )
          widthTable[k] = widthTable[k + 1];
      }
      j--;
    }
  }

  /* remove rows that are linear interpolations */
  for ( j = 1; j < height - 1; j++ )
  {
    maxLength = 0;
    for ( i = 0; i < width; i++ )
    {
      ProjectPointOntoVector(expand[j][i].pos, expand[j - 1][i].pos, expand[j + 1][i].pos, proj);
      VectorSubtract(expand[j][i].pos, proj, dir);
      len = VectorLength(dir);
      if ( len > maxLength )
        maxLength = len;
    }

    if ( maxLength < MESH_MIN_EDGE_LENGTH )
    {
      height--;
      for ( i = 0; i < width; i++ )
      {
        for ( k = j; k < height; k++ )
        {
          expand[k][i] = expand[k + 1][i];
        }
      }

      /* shift height table entries if provided */
      if ( heightTable )
      {
        for ( k = j; k < height; k++ )
          heightTable[k] = heightTable[k + 1];
      }
      j--;
    }
  }

  /* collapse the expand grid rows into contiguous memory */
  if ( height > 1 )
  {
    for ( i = 1; i < height; i++ )
    {
      memmove(&expand[0][i * width], &expand[i][0], width * sizeof(MeshVert_t));
    }
  }

  /* allocate output mesh with inline vertex data */
  totalBytes = sizeof(MeshVert_t) * width * height;
  out = malloc(sizeof(Mesh_t) + totalBytes);
  out->width = width;
  out->height = height;
  out->verts = (MeshVert_t *)(out + 1);
  memcpy(out->verts, expand, totalBytes);
  return out;
}

/*
================
LerpDrawVertAmount

Biased interpolation (amount=0 gives a, amount=1 gives b)
================
*/
void LerpDrawVertAmount(MeshVert_t *a, MeshVert_t *b, float amount, MeshVert_t *out)
{
  unsigned char *ca, *cb, *co;

  out->pos[0] = (float)LERPF(a->pos[0], b->pos[0], amount);
  out->pos[1] = (float)LERPF(a->pos[1], b->pos[1], amount);
  out->pos[2] = (float)LERPF(a->pos[2], b->pos[2], amount);
  out->st[0] = (float)LERPF(a->st[0], b->st[0], amount);
  out->st[1] = (float)LERPF(a->st[1], b->st[1], amount);
  out->lmSt[0] = (float)LERPF(a->lmSt[0], b->lmSt[0], amount);
  out->lmSt[1] = (float)LERPF(a->lmSt[1], b->lmSt[1], amount);
  ca = (unsigned char *)&a->color;
  cb = (unsigned char *)&b->color;
  co = (unsigned char *)&out->color;
  co[0] = (int)((double)(cb[0] - ca[0]) * amount + (double)ca[0]);
  co[1] = (int)((double)(cb[1] - ca[1]) * amount + (double)ca[1]);
  co[2] = (int)((double)(cb[2] - ca[2]) * amount + (double)ca[2]);
  co[3] = (int)((double)(cb[3] - ca[3]) * amount + (double)ca[3]);
  out->normal[0] = (float)LERPF(a->normal[0], b->normal[0], amount);
  out->normal[1] = (float)LERPF(a->normal[1], b->normal[1], amount);
  out->normal[2] = (float)LERPF(a->normal[2], b->normal[2], amount);
  VecNormalize(out->normal);
}

/*
================
SubdivideMeshQuads

Unused.
================
*/
Mesh_t *SubdivideMeshQuads(Mesh_t *inMesh, float subdivSize, int maxsize, int *widthTable, int *heightTable, int *widthArr, int *heightArr)
{
  int width;
  int height;
  int expandPos;
  int colIdx;
  int subdivCount;
  int maxSubdiv;
  float invSubdiv;
  float maxEdgeLen;
  float edgeLen;
  int segPlusOne;
  int i, j;
  Mesh_t *out;

  width = inMesh->width;
  height = inMesh->height;

  /* copy input mesh verts into expand grid */
  for (i = 0; i < width; i++)
  {
    for (j = 0; j < height; j++)
    {
      expand[j][i] = inMesh->verts[j * inMesh->width + i];
    }
  }

  if (maxsize > MAX_EXPANDED_AXIS)
    Com_Error("SubdivideMeshQuads: maxsize > MAX_EXPANDED_AXIS");

  /* subdivide along width axis */
  maxSubdiv = (maxsize - width) / (width - 1);
  expandPos = 0;
  colIdx = 0;

  invSubdiv = 1.0f / subdivSize;

  if ( width > 1 )
  {
    while (colIdx < width - 1)
    {
      /* find max edge length across all rows for this column */
      maxEdgeLen = 0.0f;
      for (j = 0; j < height; j++)
      {
        MeshVert_t *curVert = &expand[j][expandPos];
        MeshVert_t *nextVert = &expand[j][expandPos + 1];
        float dx = nextVert->pos[0] - curVert->pos[0];
        float dy = nextVert->pos[1] - curVert->pos[1];
        float dz = nextVert->pos[2] - curVert->pos[2];
        { vec3_t edgeV = {dx, dy, dz}; edgeLen = VectorLength(edgeV); }
        if (edgeLen > maxEdgeLen)
          maxEdgeLen = edgeLen;
      }

      /* compute subdivision count for this segment */
      subdivCount = (int)(invSubdiv * maxEdgeLen);
      if (subdivCount > maxSubdiv)
        subdivCount = maxSubdiv;

      segPlusOne = subdivCount + 1;
      if (widthTable)
        widthTable[colIdx] = segPlusOne;

      if (subdivCount > 0)
      {
        width += subdivCount;

        /* shift width array entries right to make room */
        if (widthArr)
        {
          int endIdx = width - 1;
          int startIdx = subdivCount + expandPos;
          while (endIdx >= startIdx)
          {
            widthArr[endIdx] = widthArr[endIdx - subdivCount];
            endIdx--;
          }
          if (subdivCount >= 1)
          {
            int *fillPtr = &widthArr[expandPos + 1];
            for (int k = 0; k < subdivCount; k++)
              fillPtr[k] = widthArr[expandPos];
          }
        }

        /* interpolate new vertices for each row */
        if (height > 0)
        {
          int nextCol = subdivCount + expandPos;
          for (j = 0; j < height; j++)
          {
            /* shift existing vertices right to make room */
            int lastIdx = width - 1;
            if (lastIdx > nextCol)
            {
              int shiftFrom = lastIdx - subdivCount;
              for (int si = lastIdx; si > nextCol; si--)
              {
                expand[j][si] = expand[j][shiftFrom];
                shiftFrom--;
              }
            }

            /* interpolate new vertices */
            if (subdivCount >= 1)
            {
              float totalSegs = (float)segPlusOne;
              for (int k = 1; k <= subdivCount; k++)
              {
                float frac = (float)k / totalSegs;
                LerpDrawVertAmount(&expand[j][expandPos], &expand[j][nextCol], frac, &expand[j][expandPos + k]);
              }
            }
          }
        }
      }

      expandPos += subdivCount + 1;
      colIdx++;
    }
  }

  /* subdivide along height axis */
  maxSubdiv = (maxsize - height) / (height - 1);
  expandPos = 0;
  colIdx = 0;

  if ( height > 1 )
  {
    while (colIdx < height - 1)
    {
      /* find max edge length across all columns for this row */
      maxEdgeLen = 0.0f;
      for (i = 0; i < width; i++)
      {
        float *curPos = expand[expandPos][i].pos;
        float *nextRowPos = expand[expandPos + 1][i].pos;
        float dx = nextRowPos[0] - curPos[0];
        float dy = nextRowPos[1] - curPos[1];
        float dz = nextRowPos[2] - curPos[2];
        { vec3_t edgeV = {dx, dy, dz}; edgeLen = VectorLength(edgeV); }
        if (edgeLen > maxEdgeLen)
          maxEdgeLen = edgeLen;
      }

      subdivCount = (int)(invSubdiv * maxEdgeLen);
      if (subdivCount > maxSubdiv)
        subdivCount = maxSubdiv;

      segPlusOne = subdivCount + 1;
      if (heightTable)
        heightTable[colIdx] = segPlusOne;

      if (subdivCount > 0)
      {
        height += subdivCount;

        /* shift height array entries right to make room */
        if (heightArr)
        {
          int endIdx = height - 1;
          int startIdx = subdivCount + expandPos;
          while (endIdx >= startIdx)
          {
            heightArr[endIdx] = heightArr[endIdx - subdivCount];
            endIdx--;
          }
          if (subdivCount >= 1)
          {
            int *fillPtr = &heightArr[expandPos + 1];
            for (int k = 0; k < subdivCount; k++)
              fillPtr[k] = heightArr[expandPos];
          }
        }

        /* interpolate new vertices for each column */
        if (width > 0)
        {
          int nextRow = subdivCount + expandPos;
          for (i = 0; i < width; i++)
          {
            /* shift existing vertices down to make room */
            int lastIdx = height - 1;
            if (lastIdx > nextRow)
            {
              for (int si = lastIdx; si > nextRow; si--)
              {
                expand[si][i] = expand[si - subdivCount][i];
              }
            }

            if (subdivCount >= 1)
            {
              float totalSegs = (float)segPlusOne;
              for (int k = 1; k <= subdivCount; k++)
              {
                float frac = (float)k / totalSegs;
                LerpDrawVertAmount(&expand[expandPos][i], &expand[nextRow][i], frac, &expand[expandPos + k][i]);
              }
            }
          }
        }
      }

      expandPos += subdivCount + 1;
      colIdx++;
    }
  }

  /* collapse expand grid rows into contiguous memory */
  if (height > 1)
  {
    for (i = 1; i < height; i++)
    {
      memmove(&expand[0][i * width], &expand[i][0], width * sizeof(MeshVert_t));
    }
  }

  /* allocate output mesh with inline vertex data */
  out = malloc(sizeof(Mesh_t) + sizeof(MeshVert_t) * width * height);
  out->width = width;
  out->height = height;
  out->verts = (MeshVert_t *)(out + 1);
  memcpy(out->verts, expand, sizeof(MeshVert_t) * width * height);

  return out;
}

/*
================
MakeMeshNormals

Computes vertex normals by averaging cross products of
8-directional neighbor vectors. Handles wrapping meshes.
================
*/
void MakeMeshNormals(Mesh_t *inMesh)
{
  int i, j, k, dist;
  int x, y;
  vec3_t normal, sum, base, delta, temp;
  vec3_t around[NUM_NEIGHBORS];
  int good[NUM_NEIGHBORS];
  int wrapWidth, wrapHeight;
  double len;
  MeshVert_t *dv;
  int neighbors[8][2] = {
    {0,1}, {1,1}, {1,0}, {1,-1}, {0,-1}, {-1,-1}, {-1,0}, {-1,1}
  };

  /* check if mesh wraps in width */
  wrapWidth = 0;
  for ( i = 0; i < inMesh->height; i++ )
  {
    VectorSubtract(inMesh->verts[i * inMesh->width].pos,
                   inMesh->verts[i * inMesh->width + inMesh->width - 1].pos, delta);
    len = VectorLength(delta);
    if ( len > 1.0 )
      break;
  }
  if ( i == inMesh->height )
    wrapWidth = 1;

  /* check if mesh wraps in height */
  wrapHeight = 0;
  for ( i = 0; i < inMesh->width; i++ )
  {
    VectorSubtract(inMesh->verts[i].pos,
                   inMesh->verts[i + (inMesh->height - 1) * inMesh->width].pos, delta);
    len = VectorLength(delta);
    if ( len > 1.0 )
      break;
  }
  if ( i == inMesh->width )
    wrapHeight = 1;

  /* compute normals for each vertex */
  for ( i = 0; i < inMesh->width; i++ )
  {
    for ( j = 0; j < inMesh->height; j++ )
    {
      dv = &inMesh->verts[j * inMesh->width + i];
      VectorCopy(dv->pos, base);

      /* find 8-directional neighbor vectors */
      for ( k = 0; k < 8; k++ )
      {
        VectorClear(around[k]);
        good[k] = 0;

        for ( dist = 1; dist <= 3; dist++ )
        {
          x = i + neighbors[k][0] * dist;
          y = j + neighbors[k][1] * dist;

          if ( wrapWidth )
          {
            if ( x < 0 )
              x = inMesh->width - 1 + x;
            else if ( x >= inMesh->width )
              x = 1 + x - inMesh->width;
          }

          if ( wrapHeight )
          {
            if ( y < 0 )
              y = inMesh->height - 1 + y;
            else if ( y >= inMesh->height )
              y = 1 + y - inMesh->height;
          }

          if ( x < 0 || x >= inMesh->width || y < 0 || y >= inMesh->height )
            break;

          VectorSubtract(inMesh->verts[y * inMesh->width + x].pos, base, temp);
          if ( VecNormalize(temp) == 0.0 )
            continue;

          good[k] = 1;
          VectorCopy(temp, around[k]);
          break;
        }
      }

      /* sum cross products of adjacent good neighbor pairs */
      VectorClear(sum);
      for ( k = 0; k < 8; k++ )
      {
        if ( !good[k] || !good[(k + 1) & 7] )
          continue;

        CrossProduct(around[(k + 1) & 7], around[k], normal);
        if ( VecNormalize(normal) == 0.0 )
          continue;

        VectorAdd(normal, sum, sum);
      }

      Vec3NormalizeTo(sum, dv->normal);
    }
  }
}

/*
================
GuessPatchPlane

Finds best-fit plane for patch mesh by averaging triangle normals
================
*/
int GuessPatchPlane(Mesh_t *inMesh, float *plane)
{
  int row, colIdx;
  MeshVert_t *v00, *v10, *v01, *v11;
  double avgDist, dotDist;
  float edge1[3], edge2[3], faceNormal[3];
  float sumX, sumY, sumZ;
  int numPlanes;

  /* sumNormal and dist must be volatile — original binary reloads from
     32-bit float stack each iteration. Without volatile, MSVC keeps
     values in 80-bit FPU registers, changing rounding behavior. */
	 
  volatile float sumNormal[3];
  volatile float dist;

  sumNormal[0] = 0.0;
  sumNormal[1] = 0.0;
  sumNormal[2] = 0.0;
  sumX = 0.0;
  sumY = 0.0;
  sumZ = 0.0;
  numPlanes = 0;

  /* accumulate face normals from mesh grid triangles */
  for ( row = 0; row < inMesh->height - 1; row++ )
  {
    for ( colIdx = 0; colIdx < inMesh->width - 1; colIdx++ )
    {
      v00 = &inMesh->verts[colIdx + row * inMesh->width];
      v10 = &inMesh->verts[colIdx + 1 + row * inMesh->width];
      v01 = &inMesh->verts[colIdx + (row + 1) * inMesh->width];

      /* triangle 1: v00-v10-v01 */
      VectorSubtract(v10->pos, v00->pos, edge1);
      VectorSubtract(v01->pos, v00->pos, edge2);
      CrossProduct(edge1, edge2, faceNormal);
      if ( VecNormalize(faceNormal) > MIN_NORMAL_LENGTH )
      {
        v00 = &inMesh->verts[colIdx + row * inMesh->width];
        sumNormal[0] = faceNormal[0] + sumNormal[0];
        sumNormal[1] = faceNormal[1] + sumNormal[1];
        sumNormal[2] = faceNormal[2] + sumNormal[2];
        sumX = sumX + v00->pos[0];
        sumY = sumY + v00->pos[1];
        sumZ = sumZ + v00->pos[2];
        ++numPlanes;
      }

      /* triangle 2: v11-v01-v10 */
      v01 = &inMesh->verts[colIdx + (row + 1) * inMesh->width];
      v11 = &inMesh->verts[colIdx + 1 + (row + 1) * inMesh->width];
      v10 = &inMesh->verts[colIdx + 1 + row * inMesh->width];

      VectorSubtract(v01->pos, v11->pos, edge1);
      VectorSubtract(v10->pos, v11->pos, edge2);
      CrossProduct(edge1, edge2, faceNormal);
      if ( VecNormalize(faceNormal) > MIN_NORMAL_LENGTH )
      {
        v11 = &inMesh->verts[colIdx + 1 + (row + 1) * inMesh->width];
        sumNormal[0] = faceNormal[0] + sumNormal[0];
        sumNormal[1] = faceNormal[1] + sumNormal[1];
        sumNormal[2] = faceNormal[2] + sumNormal[2];
        sumX = sumX + v11->pos[0];
        sumY = sumY + v11->pos[1];
        sumZ = sumZ + v11->pos[2];
        ++numPlanes;
      }
    }
  }

  if ( VecNormalize((float *)sumNormal) < MIN_NORMAL_LENGTH )
    return 0;

  /* compute average plane distance */
  { float sumV[3] = {sumX, sumY, sumZ};
  avgDist = DotProduct210(sumV, sumNormal) / (double)numPlanes; }
  dist = avgDist;

  /* validate all vertices lie within +/-1.0 of the plane */
  for ( colIdx = 0; colIdx < inMesh->width - 1; colIdx++ )
  {
    for ( row = 0; row < inMesh->height - 1; row++ )
    {
      MeshVert_t *cv = &inMesh->verts[colIdx + row * inMesh->width];
      dotDist = DotProduct210(sumNormal, cv->pos) - dist;
      if ( dotDist > 1.0 || dotDist < -1.0 )
        return 0;
    }
  }

  /* output the plane equation */
  if ( plane )
  {
    plane[0] = sumNormal[0];
    plane[1] = sumNormal[1];
    plane[2] = sumNormal[2];
    plane[3] = dist;
  }
  return 1;
}

/*
================
SubdivideMesh

Iteratively subdivides mesh curves until the error between control
points and their subdivided midpoints is below maxError, or the span

NOTE: volatile float midpoint/edgeNext variables preserve FPU precision
matching — the original binary stores these to 32-bit float (fstp dword)
between computations. Without volatile, MSVC keeps them in 80-bit FPU
registers, producing different rounding.
================
*/
Mesh_t *SubdivideMesh(int inWidth, int inHeight, int a3, MeshVert_t *inVerts, int a5, float maxError, float minLength, int *widthTable, int *heightTable)
{
  int i, j, k;
  MeshVert_t prev, next, mid;
  int width, height;
  unsigned int totalBytes;
  Mesh_t *out;

  volatile float midX, midY, midZ;
  volatile float midX2, midY2, midZ2;
  volatile float edgeNextX, edgeNextY, edgeNextZ;
  volatile float edgeNextX2, edgeNextY2, edgeNextZ2;

  double edgePrevX, edgePrevY, edgePrevZ;
  double errorDistX, errorDistY;
  double edgePrevX2, edgePrevY2, edgePrevZ2;
  double errorDistX2, errorDistY2;

  width = inWidth;
  height = inHeight;

  /* copy input verts into expand grid */
  for ( i = 0; i < inWidth; i++ )
  {
    for ( j = 0; j < inHeight; j++ )
    {
      expand[j][i] = inVerts[j * inWidth + i];
    }
  }

  /* initialize tracking tables with identity mapping */
  if ( heightTable )
  {
    for ( i = 0; i < height; i++ )
      heightTable[i] = i;
  }
  if ( widthTable )
  {
    for ( j = 0; j < width; j++ )
      widthTable[j] = j;
  }

  /* horizontal subdivisions */
  for ( j = 0; j + 2 < width; j += 2 )
  {
    /* check subdivided midpoints against control points */
    for ( i = 0; i < height; i++ )
    {
      /* check if span to previous control point is too long */
      edgePrevX = expand[i][j + 1].pos[0] - expand[i][j].pos[0];
      edgePrevY = expand[i][j + 1].pos[1] - expand[i][j].pos[1];
      edgePrevZ = expand[i][j + 1].pos[2] - expand[i][j].pos[2];
      if ( sqrt(MulAdd2(edgePrevZ,edgePrevZ, edgePrevY,edgePrevY) + (double)edgePrevX*edgePrevX) > minLength )
        break;

      /* check if span to next control point is too long */
      edgeNextX = expand[i][j + 2].pos[0] - expand[i][j + 1].pos[0];
      edgeNextY = expand[i][j + 2].pos[1] - expand[i][j + 1].pos[1];
      edgeNextZ = expand[i][j + 2].pos[2] - expand[i][j + 1].pos[2];
      if ( sqrt(MulAdd2(edgeNextZ,edgeNextZ, edgeNextY,edgeNextY) + (double)edgeNextX*edgeNextX) > minLength )
        break;

      /* check if midpoint error exceeds threshold */
      midY = ((double)expand[i][j + 1].pos[1] + expand[i][j + 1].pos[1] + expand[i][j + 2].pos[1] + expand[i][j].pos[1]) * 0.25;
      errorDistY = expand[i][j + 1].pos[1] - midY;
      midX = ((double)expand[i][j + 1].pos[0] + expand[i][j + 1].pos[0] + expand[i][j + 2].pos[0] + expand[i][j].pos[0]) * 0.25;
      errorDistX = expand[i][j + 1].pos[0] - midX;
      midZ = ((double)expand[i][j + 1].pos[2] + expand[i][j + 1].pos[2] + expand[i][j + 2].pos[2] + expand[i][j].pos[2]) * 0.25;
      { double errZ = (double)expand[i][j + 1].pos[2] - (double)midZ;
      if ( sqrt(MulAdd2(errZ,errZ, errorDistY,errorDistY) + (double)errorDistX*errorDistX) > maxError )
        break; }
    }

    if ( width + 2 >= MAX_EXPANDED_AXIS )
      break;

    if ( i == height )
      continue;

    /* insert two columns and replace the peak */
    width += 2;

    if ( widthTable )
    {
      for ( k = width - 1; k > j + 3; k-- )
        widthTable[k] = widthTable[k - 2];
      int oldVal = widthTable[j + 1];
      widthTable[j + 3] = oldVal;
      widthTable[j + 2] = oldVal;
      widthTable[j + 1] = widthTable[j];
    }

    for ( i = 0; i < height; i++ )
    {
      LerpDrawVert(&expand[i][j], &expand[i][j + 1], &prev);
      LerpDrawVert(&expand[i][j + 1], &expand[i][j + 2], &next);
      LerpDrawVert(&prev, &next, &mid);

      for ( k = width - 1; k > j + 3; k-- )
        expand[i][k] = expand[i][k - 2];

      expand[i][j + 1] = prev;
      expand[i][j + 2] = mid;
      expand[i][j + 3] = next;
    }

    /* back up and recheck this set */
    j -= 2;
  }

  /* vertical subdivisions */
  for ( j = 0; j + 2 < height; j += 2 )
  {
    /* check subdivided midpoints against control points */
    for ( i = 0; i < width; i++ )
    {
      /* check if span to previous control point is too long */
      edgePrevX2 = expand[j + 1][i].pos[0] - expand[j][i].pos[0];
      edgePrevY2 = expand[j + 1][i].pos[1] - expand[j][i].pos[1];
      edgePrevZ2 = expand[j + 1][i].pos[2] - expand[j][i].pos[2];
      if ( sqrt(MulAdd2(edgePrevZ2,edgePrevZ2, edgePrevY2,edgePrevY2) + (double)edgePrevX2*edgePrevX2) > minLength )
        break;

      /* check if span to next control point is too long */
      edgeNextX2 = expand[j + 2][i].pos[0] - expand[j + 1][i].pos[0];
      edgeNextY2 = expand[j + 2][i].pos[1] - expand[j + 1][i].pos[1];
      edgeNextZ2 = expand[j + 2][i].pos[2] - expand[j + 1][i].pos[2];
      if ( sqrt(MulAdd2(edgeNextZ2,edgeNextZ2, edgeNextY2,edgeNextY2) + (double)edgeNextX2*edgeNextX2) > minLength )
        break;

      /* check if midpoint error exceeds threshold */
      midY2 = ((double)expand[j + 1][i].pos[1] + expand[j + 1][i].pos[1] + expand[j + 2][i].pos[1] + expand[j][i].pos[1]) * 0.25;
      errorDistY2 = expand[j + 1][i].pos[1] - midY2;
      midX2 = ((double)expand[j + 1][i].pos[0] + expand[j + 1][i].pos[0] + expand[j + 2][i].pos[0] + expand[j][i].pos[0]) * 0.25;
      errorDistX2 = expand[j + 1][i].pos[0] - midX2;
      midZ2 = ((double)expand[j + 1][i].pos[2] + expand[j + 1][i].pos[2] + expand[j + 2][i].pos[2] + expand[j][i].pos[2]) * 0.25;
      { double errZ2 = (double)expand[j + 1][i].pos[2] - (double)midZ2;
      if ( sqrt(MulAdd2(errZ2,errZ2, errorDistY2,errorDistY2) + (double)errorDistX2*errorDistX2) > maxError )
        break; }
    }

    if ( height + 2 >= MAX_EXPANDED_AXIS )
      break;

    if ( i == width )
      continue;

    /* insert two rows and replace the peak */
    height += 2;

    if ( heightTable )
    {
      for ( k = height - 1; k > j + 3; k-- )
        heightTable[k] = heightTable[k - 2];
      int oldVal = heightTable[j + 1];
      heightTable[j + 3] = oldVal;
      heightTable[j + 2] = oldVal;
      heightTable[j + 1] = heightTable[j];
    }

    for ( i = 0; i < width; i++ )
    {
      LerpDrawVert(&expand[j][i], &expand[j + 1][i], &prev);
      LerpDrawVert(&expand[j + 1][i], &expand[j + 2][i], &next);
      LerpDrawVert(&prev, &next, &mid);

      for ( k = height - 1; k > j + 3; k-- )
        expand[k][i] = expand[k - 2][i];

      expand[j + 1][i] = prev;
      expand[j + 2][i] = mid;
      expand[j + 3][i] = next;
    }

    /* back up and recheck this set */
    j -= 2;
  }

  /* collapse expand grid rows into contiguous memory */
  if ( height > 1 )
  {
    for ( i = 1; i < height; i++ )
    {
      memmove(&expand[0][i * width], &expand[i][0], width * sizeof(MeshVert_t));
    }
  }

  /* allocate output mesh with inline vertex data */
  totalBytes = sizeof(MeshVert_t) * width * height;
  out = malloc(sizeof(Mesh_t) + totalBytes);
  out->width = width;
  out->height = height;
  out->verts = (MeshVert_t *)(out + 1);
  memcpy(out->verts, expand, totalBytes);
  return out;
}

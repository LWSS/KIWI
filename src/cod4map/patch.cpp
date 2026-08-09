/*
patch.c — Patch and terrain surface processing

Reconstructed from cod2map.exe by Rose.
*/

#include "cod4map.h"

char s_assertDisable_FindSharedVertex;
char s_assertDisable_ParsePatchVertex;
char s_assertDisable_MergePatchVertexPositions;
char s_assertDisable_SubdivideTerrain;
char s_assertDisable_SubdivideTerrain;
char s_assertDisable_SubdivideTerrain;
char s_assertDisable_SubdivideTerrain;
char s_assertDisable_SubdivideTerrain;
char s_assertDisable_SubdivideTerrain;


/*
================
PatchMapDrawSurfs

Process patches/terrain for an entity.
Creates terrain draw surfaces that trigger collision processing.
================
*/
void PatchMapDrawSurfs(Entity_t *entity)
{
  Patch_t *patch;
  Patch_t *p1, *p2, *pp;
  unsigned char *connMatrix;
  unsigned int numPatches;
  unsigned int i, j, k;
  int numVerts1, numVerts2;
  MeshVert_t *verts1, *verts2;
  int matchCount;
  int found;
  char *connectedFlags;
  int *processedFlags;
  Patch_t **patchArray;
  DrawSurf_t *surf;
  unsigned int groupCount;
  int v1Idx;

  Com_DPrintf("----- PatchMapDrawSurfs -----\n");

  patch = entity->patches;
  numPatches = 0;

  if ( !patch )
      return;

  patchArray = malloc(MAX_MAP_PATCHES * sizeof(*patchArray));
  if ( !patchArray ) return;

  do {
      if ( patch->patchType == 0 ) {
          patchArray[numPatches] = patch;
          numPatches++;
      }
      patch = patch->next;
  } while ( patch );

  if (numPatches == 0) {
      free(patchArray);
      return;
  }

  connMatrix = malloc(numPatches * numPatches);
  if (!connMatrix) {
      free(patchArray);
      return;
  }
  memset(connMatrix, 0, numPatches * numPatches);

  for (i = 0; i < numPatches; i++) {
      connMatrix[i * numPatches + i] = 1;

      p1 = patchArray[i];
      numVerts1 = p1->height * p1->width;
      verts1 = p1->vertexData;

      for (j = i + 1; j < numPatches; j++) {
          p2 = patchArray[j];
          numVerts2 = p2->height * p2->width;
          verts2 = p2->vertexData;

          matchCount = 0;

          #define PATCH_CONNECT_EPSILON 1.0f
          for (k = 0; k < (unsigned int)numVerts2; k++) {
              found = 0;
              for ( v1Idx = 0; v1Idx < numVerts1; v1Idx++ )
              {
                  if ( fabs(verts2[k].pos[0] - verts1[v1Idx].pos[0]) < PATCH_CONNECT_EPSILON &&
                       fabs(verts2[k].pos[1] - verts1[v1Idx].pos[1]) < PATCH_CONNECT_EPSILON &&
                       fabs(verts2[k].pos[2] - verts1[v1Idx].pos[2]) < PATCH_CONNECT_EPSILON )
                  {
                      found = 1;
                      break;
                  }
              }

              if (found) break;
              matchCount++;
          }

          if (matchCount == numVerts2) {
              connMatrix[i * numPatches + j] = 0;
              connMatrix[j * numPatches + i] = 0;
          } else {
              connMatrix[i * numPatches + j] = 1;
              connMatrix[j * numPatches + i] = 1;
          }
      }
  }

  processedFlags = malloc(MAX_MAP_PATCHES * sizeof(*processedFlags));
  if (!processedFlags) {
      free(connMatrix);
      free(patchArray);
      return;
  }
  memset(processedFlags, 0, MAX_MAP_PATCHES * sizeof(*processedFlags));

  connectedFlags = malloc(numPatches);
  if (!connectedFlags) {
      free(processedFlags);
      free(connMatrix);
      free(patchArray);
      return;
  }

  groupCount = 0;
  for (i = 0; i < numPatches; i++) {
      if (!processedFlags[i])
          groupCount++;

      memset(connectedFlags, 0, numPatches);
      GrowGroup_r(i, numPatches, connMatrix, connectedFlags);

      for (j = 0; j < numPatches; j++) {
          if (connectedFlags[j] != 0)
              processedFlags[j] = 1;
      }

      pp = patchArray[i];

      surf = DrawSurfaceForPatch(pp);
      surf->shaderInfo = pp->material;
      surf->materialName = (const char *)pp->lmMaterial;
      surf->surfaceFlags = pp->surfaceFlags;
      surf->contentFlags = pp->contentFlags;

      CenterPatchLightmapCoords(surf);
  }

  Com_DPrintf("%5i patches\n", numPatches);
  Com_DPrintf("%5i patch LOD groups\n", groupCount);

  free(connectedFlags);
  free(processedFlags);
  free(connMatrix);
  free(patchArray);
}

/*
================
GrowGroup_r

Recursively flood-fills connected patches into a group
================
*/
void GrowGroup_r(int patchIdx, int patchCount, unsigned char *bordering, char *group)
{
  int j;
  unsigned char *borderRow;

  if ( group[patchIdx] )
    return;

  group[patchIdx] = 1;
  borderRow = bordering + patchCount * patchIdx;
  for ( j = 0; j < patchCount; ++j )
  {
    if ( borderRow[j] )
      GrowGroup_r(j, patchCount, bordering, group);
  }
}

/*
================
ComparePatchSurfaces

qsort comparator for terrain patches (by flags, material, subdiv)
================
*/
int ComparePatchSurfaces(const void *elemA, const void *elemB)
{
  Patch_t *a = *(Patch_t **)elemA;
  Patch_t *b = *(Patch_t **)elemB;

  /* sort by surface flags first */
  if ( a->surfaceFlags != b->surfaceFlags )
    return a->surfaceFlags - b->surfaceFlags;

  /* then by content flags */
  if ( a->contentFlags != b->contentFlags )
    return a->contentFlags - b->contentFlags;

  /* CoD4's comparator uses the raw material-pool stride (64 bytes), then
     the separate terrain grouping key at Patch_t+60. */
  if ( a->material == b->material )
  {
    if ( a->lmMaterial == b->lmMaterial )
      return a->terrainSortKey - b->terrainSortKey;
    return (int)(((intptr_t)a->lmMaterial - (intptr_t)b->lmMaterial) >> 6);
  }
  
  return (int)(((intptr_t)a->material - (intptr_t)b->material) >> 6);
}

/*
================
StableSortPatchSurfaces

The native CoD4 CRT qsort keeps equal terrain patches in parser-list order.
The current toolchain's qsort does not, which changes DrawSurf allocation and
therefore collision vertex/triangle order.  Preserve the native observable
order explicitly while retaining ComparePatchSurfaces as the ordering rule.
================
*/
static void StableSortPatchSurfaces(Patch_t **patches, int patchCount)
{
  int i;

  for ( i = 1; i < patchCount; ++i )
  {
    Patch_t *patch = patches[i];
    int j = i;

    while ( j > 0 && ComparePatchSurfaces(&patches[j - 1], &patch) > 0 )
    {
      patches[j] = patches[j - 1];
      --j;
    }
    patches[j] = patch;
  }
}

/*
================
CenterPatchLightmapCoords

Centers lightmap UVs around integer boundary
================
*/
void CenterPatchLightmapCoords(DrawSurf_t *surf)
{
  int i;
  int numVerts;
  MeshVert_t *mv;
  double maxU;
  float maxV, minU, minV;
  float centerU, centerV, offsetU, offsetV;

  if ( !surf || !surf->shaderInfo || !surf->shaderInfo->globalTexture )
    return;

  mv = surf->verts;
  numVerts = surf->numVerts;

  /* find min/max texture coordinate bounds */
  minU = mv[0].st[0];
  maxU = mv[0].st[0];
  minV = mv[0].st[1];
  maxV = mv[0].st[1];

  for ( i = 1; i < numVerts; i++ )
  {
    if ( mv[i].st[0] < minU )
      minU = mv[i].st[0];
    if ( mv[i].st[0] > maxU )
      maxU = mv[i].st[0];
    if ( mv[i].st[1] < minV )
      minV = mv[i].st[1];
    if ( mv[i].st[1] > maxV )
      maxV = mv[i].st[1];
  }

  /* compute center and snap to integer boundary */
  centerU = MIDF(maxU, minU);
  centerV = MIDF(maxV, minV);
  offsetU = (float)fistp_sub((float)centerU, FISTP_HALF_BIAS);
  offsetV = (float)fistp_sub((float)centerV, FISTP_HALF_BIAS);

  /* shift all texture coordinates by the offset */
  for ( i = 0; i < numVerts; i++ )
  {
    mv[i].st[0] -= offsetU;
    mv[i].st[1] -= offsetV;
  }
}

/*
================
DrawSurfaceForPatch

Creates a draw surface from a parsed patch
================
*/
DrawSurf_t *DrawSurfaceForPatch(Patch_t *pt)
{
  Mesh_t *mesh;
  DrawSurf_t *surf;
  MapDrawSurf_t *nativeDrawSurf;
  float plane[4];
  int hasPlane, i, j, idx, totalVerts;

  mesh = CopyMesh((Mesh_t *)&pt->width);
  PutMeshOnCurve(mesh->width, mesh->height, mesh->reserved, mesh->verts);
  MakeMeshNormals(mesh);

  /* copy computed normals back to patch vertex data */
  for ( i = 0; i < pt->width; i++ )
  {
    for ( j = 0; j < pt->height; j++ )
    {
      idx = j * pt->width + i;
      VectorCopy(mesh->verts[idx].normal, pt->vertexData[idx].normal);
    }
  }

  hasPlane = GuessPatchPlane(mesh, plane);
  FreeMesh(mesh);

  /* allocate and fill draw surface */
  surf = AllocDrawSurface();
  surf->entityNum = pt->entityNum;
  surf->brushNum = pt->smoothing;
  surf->mapName = pt->mapName;
  surf->mapInfoIndex = pt->mapInfoIndex;
  surf->mapBrush = 0;
  surf->sideRef = 0;
  surf->surfaceFlags = pt->surfaceFlags;
  surf->contentFlags = pt->contentFlags;
  if ( hasPlane )
    surf->planeNum = FindFloatPlane(plane, plane[3]);
  surf->isPatch = 1;
  surf->patchType = pt->patchType;
  surf->hasPatchPlane = hasPlane;
  surf->subdivLevel = pt->subdivLevel;
  surf->patchWidth = pt->width;
  surf->patchHeight = pt->height;
  totalVerts = pt->width * pt->height;
  surf->numVerts = totalVerts;
  surf->verts = malloc(sizeof(MeshVert_t) * totalVerts);
  memcpy(surf->verts, pt->vertexData, sizeof(MeshVert_t) * totalVerts);
  surf->lightStyle = LIGHTSTYLE_NONE;
  surf->fogNum = -1;
  surf->outputNum = pt->outputNum;

  /* Native 0x42A9F0 MapDrawSurf carrier. */
  nativeDrawSurf = NativeMapDrawSurfFor(surf);
  nativeDrawSurf->material = pt->material;
  nativeDrawSurf->lightmapMaterial = pt->lmMaterial;
  nativeDrawSurf->contentFlags = pt->surfaceFlags;
  nativeDrawSurf->toolFlags = pt->contentFlags;
  nativeDrawSurf->compileOrSmoothing = pt->smoothing;
  nativeDrawSurf->entityNum = pt->entityNum;
  nativeDrawSurf->sourceIndex.mapInfoIndex = pt->mapInfoIndex;
  nativeDrawSurf->sourceInfo.mapName = pt->mapName;
  nativeDrawSurf->vertCount = surf->numVerts;
  nativeDrawSurf->verts = surf->verts;
  nativeDrawSurf->outputNum = pt->outputNum;
  nativeDrawSurf->isPatch = 1;
  nativeDrawSurf->isTerrain = 0;
  nativeDrawSurf->u.patch.width = pt->width;
  nativeDrawSurf->u.patch.height = pt->height;
  nativeDrawSurf->u.patch.subdivLevel = pt->subdivLevel;

  return surf;
}

/*
================
ParsePatchVertex

Parses a single vertex from map file (position, color, texcoords)
================
*/
int ParsePatchVertex(char **parsePtr, float *matrix, float scale, MeshVert_t *vert)
{
  unsigned char *color;
  float pos[3];

  Assert(vert, s_assertDisable_ParsePatchVertex);
  COM_MatchToken(parsePtr, "v", 0);
  pos[0] = COM_ParseFloat(parsePtr);
  pos[1] = COM_ParseFloat(parsePtr);
  pos[2] = COM_ParseFloat(parsePtr);
  MatrixVecScaleAddTransform43(matrix, scale, pos, vert->pos);
  color = (unsigned char *)&vert->color;
  if ( !strcmp(COM_Parse(parsePtr), "c") )
  {
    color[2] = COM_ParseIntExt(parsePtr);  /* B */
    color[1] = COM_ParseIntExt(parsePtr);  /* G */
    color[0] = COM_ParseIntExt(parsePtr);  /* R */
    color[3] = COM_ParseIntExt(parsePtr);  /* A */
  }
  else
  {
    /* no color token — use white defaults */
    Com_UngetToken();
    color[2] = 0xFF;
    color[1] = 0xFF;
    color[0] = 0xFF;
    color[3] = 0xFF;
  }
  COM_MatchToken(parsePtr, "t", 0);
  float parsedTexU = COM_ParseFloat(parsePtr);
  float parsedTexV = COM_ParseFloat(parsePtr);
  float parsedLmapU = COM_ParseFloat(parsePtr);
  float parsedLmapV = COM_ParseFloat(parsePtr);

  /* convert from 0-1024 map UV space to 0-1 normalized */
  #define UV_SCALE (1.0f / 1024.0f)
  vert->st[0] = parsedTexU * UV_SCALE;
  vert->st[1] = parsedTexV * UV_SCALE;
  vert->lmSt[0] = (float)((double)parsedLmapU * sampleScale * UV_SCALE);
  vert->lmSt[1] = (float)((double)parsedLmapV * sampleScale * UV_SCALE);

  if ( !strcmp(COM_Parse(parsePtr), "f") )
    return COM_ParseIntExt(parsePtr);

  /* no flags token */
  Com_UngetToken();
  return 0;
}

/*
================
ParsePatch

Parses a mesh/curve from the map file
================
*/
#define MAX_PATCH_SIZE 32

static void ParsePatchLayer(char **parsePtr)
{
  char layerName[1028];
  char *token = COM_Parse(parsePtr);

  if (!strcmp(token, "layer"))
  {
    token = COM_ParseExt(parsePtr);
    strcpy(layerName, *token ? token : "000_Global");
    return;
  }

  Com_UngetToken();
  strcpy(layerName, "000_Global");
}

static int ParsePatchSmoothing(char **parsePtr)
{
  char smoothingName[1028];
  char *token = COM_Parse(parsePtr);

  if (!strcmp(token, "smoothing"))
  {
    token = COM_ParseExt(parsePtr);
    strcpy(smoothingName, *token ? token : "smoothing_smooth");
  }
  else
  {
    Com_UngetToken();
    strcpy(smoothingName, "smoothing_smooth");
  }

  if (!_stricmp(smoothingName, "smoothing_hard"))
    return 0;
  if (!_stricmp(smoothingName, "smoothing_smooth"))
    return 1;
  if (!_stricmp(smoothingName, "smoothing_smooth_other"))
    return 2;

  Com_Error("ParseSmoothing(): Unknown smoothing name '%s'", smoothingName);
  return 0;
}

/* Native patch loading merges control points that are equal within 0.01,
   averaging every member into its earliest matching vertex. */
static void MergePatchVertexPositions(MeshVert_t *verts, int vertCount)
{
  int vertMap[MAX_PATCH_SIZE * MAX_PATCH_SIZE];
  int groupSize[MAX_PATCH_SIZE * MAX_PATCH_SIZE];
  vec3_t groupSum[MAX_PATCH_SIZE * MAX_PATCH_SIZE];
  int vertIter, priorIter, root;

  for (vertIter = 0; vertIter < vertCount; ++vertIter)
  {
    vertMap[vertIter] = vertIter;
    groupSize[vertIter] = 1;
    VectorCopy(verts[vertIter].pos, groupSum[vertIter]);

    for (priorIter = 0; priorIter < vertIter; ++priorIter)
    {
      root = vertMap[priorIter];
      if (VectorCompareEpsilon(verts[root].pos, verts[vertIter].pos, 0.01f, 3))
      {
        vertMap[vertIter] = root;
        ++groupSize[root];
        VectorAdd(groupSum[vertIter], groupSum[root], groupSum[root]);
      }
    }
  }

  for (vertIter = 0; vertIter < vertCount; ++vertIter)
  {
    root = vertMap[vertIter];
    Assert(root <= vertIter, s_assertDisable_MergePatchVertexPositions);
    if (root == vertIter && groupSize[root] != 1)
    {
      groupSum[root][0] /= groupSize[root];
      groupSum[root][1] /= groupSize[root];
      groupSum[root][2] /= groupSize[root];
    }
    VectorCopy(groupSum[root], verts[vertIter].pos);
  }
}

/* The native parser converts the 0x60 tool-flag patch color convention only
   when every control point retains an opaque alpha. */
static void NormalizePatchVertexColors(Patch_t *patch)
{
  int vertIter;
  int vertCount;

  if ((patch->contentFlags & 0x70) != 0x60)
    return;

  vertCount = patch->width * patch->height;
  for (vertIter = 0; vertIter < vertCount; ++vertIter)
  {
    if (((unsigned char *)&patch->vertexData[vertIter].color)[3] != 255)
      return;
  }

  for (vertIter = 0; vertIter < vertCount; ++vertIter)
  {
    unsigned char *color = (unsigned char *)&patch->vertexData[vertIter].color;
    color[3] = (color[2] + color[1] + color[0]) / 3;
    color[0] = 255;
    color[1] = 255;
    color[2] = 255;
  }
}

Patch_t *ParsePatch(double unused, char **parsePtr, int patchType, float *transformMtx, float brushScale, const char *mapName)
{
  Patch_t *patch;
  MeshVert_t *verts;
  int *indices;
  int width, height, subdivLevel, col, row;
  short brushFlags;
  int toolFlags;
  int smoothing;
  char matName[MAX_QPATH], lmMatName[MAX_QPATH];

  COM_MatchToken(parsePtr, "{", 0);

  ParsePatchLayer(parsePtr);
  brushFlags = ParseContentsFlags(parsePtr);
  toolFlags = ParseToolFlags(parsePtr);
  Com_SetSpaceDelimited(1);
  strcpy(matName, COM_Parse(parsePtr));
  strcpy(lmMatName, COM_Parse(parsePtr));
  Com_SetSpaceDelimited(0);
  smoothing = ParsePatchSmoothing(parsePtr);

  width = COM_ParseInt(parsePtr);
  height = COM_ParseInt(parsePtr);
  COM_ParseInt(parsePtr);
  subdivLevel = COM_ParseInt(parsePtr);

  if ( (unsigned)width > MAX_PATCH_SIZE || (unsigned)height > MAX_PATCH_SIZE )
    Com_Error("ParsePatch: bad size %i x %i", width, height);

  verts = malloc(sizeof(MeshVert_t) * width * height);
  indices = malloc(sizeof(int) * width * height);

  /* parse vertices in column-major order */
  for ( col = 0; col < width; col++ )
  {
    COM_MatchToken(parsePtr, "(", 0);
    for ( row = 0; row < height; row++ )
    {
      indices[row * width + col] = ParsePatchVertex(parsePtr, transformMtx, brushScale, &verts[row * width + col]);
    }
    COM_MatchToken(parsePtr, ")", 0);
  }

  COM_MatchToken(parsePtr, "}", 0);
  COM_MatchToken(parsePtr, "}", 0);

  if ( noCurveBrushes )
    return NULL;

  MergePatchVertexPositions(verts, width * height);

  /* create patch structure */
  patch = malloc(sizeof(Patch_t));
  memset(patch, 0, sizeof(Patch_t));
  patch->outputNum = -1;
  patch->terrainSortKey = -1;
  patch->material = LoadMaterial(matName);
  patch->lmMaterial = LoadMaterial(lmMatName);
  patch->width = width;
  patch->height = height;
  patch->subdivLevel = subdivLevel;
  patch->vertexData = verts;
  patch->indexData = indices;
  patch->patchType = patchType;
  patch->smoothing = smoothing;
  patch->mapInfoIndex = g_currentEntity->mapInfoIndex;
  patch->entityNum = g_currentEntity->entityNum;
  patch->mapName = mapName;

  /* set surface flags from material, apply brush flags */
  patch->surfaceFlags = patch->material->contentFlags;
  if ( brushFlags & (CONTENTS_CLIPSHOT | CONTENTS_MISSILECLIP) )
    patch->surfaceFlags = patch->material->contentFlags & ~(CONTENTS_DETAIL | CONTENTS_SOLID);
  if ( brushFlags & CONTENTS_CLIPSHOT )
    patch->surfaceFlags |= CONTENTS_CLIPSHOT;
  if ( brushFlags & CONTENTS_MISSILECLIP )
    patch->surfaceFlags |= CONTENTS_MISSILECLIP;
  if ( brushFlags & CONTENTS_NONCOLLIDING )
    patch->surfaceFlags |= CONTENTS_NONCOLLIDING;
  patch->contentFlags = toolFlags | patch->material->toolFlagsWord;
  NormalizePatchVertexColors(patch);

  /* link into entity's patch list */
  patch->next = g_currentEntity->patches;
  g_currentEntity->patches = patch;

  return g_currentEntity->patches;
}

/*
================
FindSharedVertex

Finds existing vertex in pool or adds new one (vertex dedup)
================
*/
int FindSharedVertex(int *vertCount, MeshVert_t *srcVert, MeshVert_t *pool)
{
  int i;

  /* search for matching vertex */
  for ( i = 0; i < *vertCount; i++ )
  {
    if ( VectorCompareEpsilon(pool[i].pos, srcVert->pos, PLANESIDE_EPSILON, 3)
      && fabs(pool[i].st[0] - srcVert->st[0]) <= 0.00001
      && fabs(pool[i].st[1] - srcVert->st[1]) <= 0.00001 )
      return i;
  }

  /* not found — add new vertex */
  Assert(i == *vertCount, s_assertDisable_FindSharedVertex);
  (*vertCount)++;
  memcpy(&pool[i], srcVert, sizeof(MeshVert_t));
  pool[i].normal[0] = 0.0f;
  pool[i].normal[1] = 0.0f;
  pool[i].normal[2] = 0.0f;
  return i;
}

/*
================
EmitTriangle

Emits one triangle, accumulates face normal onto vertex normals
================
*/
void EmitTriangle(MeshVert_t *verts, unsigned short *indexes, unsigned int *numTris, unsigned short idx0, unsigned short idx1, unsigned short idx2)
{
  MeshVert_t *v0, *v1, *v2;
  float plane[4];
  int triBase;

  if ( idx0 == idx1 || idx1 == idx2 || idx2 == idx0 )
    return;

  v0 = &verts[idx0];
  v1 = &verts[idx1];
  v2 = &verts[idx2];

  if ( !PlaneFromPoints(plane, v0->pos, v1->pos, v2->pos) )
    return;

  /* accumulate face normal onto vertex normals */
  VectorAdd(v0->normal, plane, v0->normal);
  VectorAdd(v1->normal, plane, v1->normal);
  VectorAdd(v2->normal, plane, v2->normal);

  /* emit triangle indices */
  triBase = 3 * *numTris;
  indexes[triBase] = idx0;
  indexes[triBase + 1] = idx1;
  indexes[triBase + 2] = idx2;
  ++*numTris;
}

/*
================
MeshToTriangles

Converts mesh grid into indexed triangles with vertex dedup
================
*/
int MeshToTriangles(MeshVert_t *drawVerts, unsigned int *numIndexes, Patch_t *mesh, int *numVerts, unsigned short *indexes)
{
  int w, base, nextBase;
  unsigned short t0, t1, t2, a, b;
  int *idxData;
  int i, row, col;
  
  /* static to avoid 512KB stack allocation */
  static unsigned short vertexMap[MAX_MAP_PATCHES];

  *numIndexes = 0;
  *numVerts = 0;

  /* map each mesh vertex to shared vertex pool */
  for ( i = 0; i < mesh->width * mesh->height; i++ )
    vertexMap[i] = FindSharedVertex(numVerts, &mesh->vertexData[i], drawVerts);

  /* emit two triangles per grid cell */
  idxData = mesh->indexData;
  for ( row = 0; row < mesh->height - 1; row++ )
  {
    for ( col = 0; col < mesh->width - 1; col++ )
    {
      w = mesh->width;
      base = col + row * w;
      nextBase = base + w;

      if ( idxData[base] & 1 )
      {
        a = vertexMap[base];
        b = vertexMap[nextBase + 1];
        EmitTriangle(drawVerts, indexes, numIndexes, a, b, vertexMap[base + 1]);
        t0 = a;
        t1 = b;
        t2 = vertexMap[nextBase];
      }
      else
      {
        a = vertexMap[nextBase];
        b = vertexMap[base + 1];
        /* Native 0x4295A0 emits (bottom-left, top-right, top-left). */
        EmitTriangle(drawVerts, indexes, numIndexes, a, b, vertexMap[base]);
        t0 = a;
        t1 = b;
        t2 = vertexMap[nextBase + 1];
      }
      EmitTriangle(drawVerts, indexes, numIndexes, t1, t0, t2);
    }
  }

  /* normalize all accumulated vertex normals */
  for ( i = 0; i < *numVerts; i++ )
    VecNormalize(drawVerts[i].normal);

  return *numVerts;
}

/*
================
DrawSurfaceForTerrain

Creates a draw surface from terrain subdivision data
================
*/
DrawSurf_t *DrawSurfaceForTerrain(TerrainNode_t *terrain, Patch_t *sourcePatch)
{
  DrawSurf_t *surf;
  MapDrawSurf_t *nativeDrawSurf;
  int i;

  surf = AllocDrawSurface();
  /* Native 0x42A4F0 receives the leaf and its originating Patch_t
     separately. Its draw surface inherits the source identity carriers,
     rather than synthetic terrain defaults. */
  surf->entityNum = sourcePatch->entityNum;
  surf->brushNum = sourcePatch->smoothing;
  surf->mapName = sourcePatch->mapName;
  surf->mapInfoIndex = sourcePatch->mapInfoIndex;
  surf->shaderInfo = terrain->shaderInfo;
  surf->materialName = terrain->materialName;
  surf->surfaceFlags = terrain->surfaceFlags;
  surf->contentFlags = terrain->contentFlags;
  surf->mapBrush = 0;
  surf->sideRef = 0;
  surf->isTerrain = 1;

  /* copy index data */
  surf->numIndexes = terrain->numIndexes;
  surf->indexes = malloc(sizeof(int) * surf->numIndexes);
  for ( i = 0; i < surf->numIndexes; i++ )
    surf->indexes[i] = terrain->indexes[i];

  /* copy vertex data */
  surf->numVerts = terrain->numVerts;
  surf->verts = malloc(sizeof(MeshVert_t) * surf->numVerts);
  memcpy(surf->verts, terrain->verts, sizeof(MeshVert_t) * surf->numVerts);

  CenterPatchLightmapCoords(surf);
  surf->lightStyle = LIGHTSTYLE_NONE;
  surf->fogNum = -1;
  surf->outputNum = terrain->outputNum;

  /* Native 0x42A4F0 MapDrawSurf carrier.  Its indexed union uses 32-bit
     indices; the expanded DrawSurf_t has already performed that conversion. */
  nativeDrawSurf = NativeMapDrawSurfFor(surf);
  nativeDrawSurf->material = terrain->shaderInfo;
  nativeDrawSurf->lightmapMaterial = (void *)terrain->materialName;
  nativeDrawSurf->contentFlags = terrain->surfaceFlags;
  nativeDrawSurf->toolFlags = terrain->contentFlags;
  nativeDrawSurf->compileOrSmoothing = terrain->smoothing;
  nativeDrawSurf->entityNum = sourcePatch->entityNum;
  nativeDrawSurf->sourceIndex.mapInfoIndex = sourcePatch->mapInfoIndex;
  nativeDrawSurf->sourceInfo.mapName = sourcePatch->mapName;
  nativeDrawSurf->vertCount = surf->numVerts;
  nativeDrawSurf->verts = surf->verts;
  nativeDrawSurf->outputNum = terrain->outputNum;
  nativeDrawSurf->isPatch = 0;
  nativeDrawSurf->isTerrain = 1;
  nativeDrawSurf->u.indexed.indexCount = surf->numIndexes;
  nativeDrawSurf->u.indexed.indexes = surf->indexes;

  return surf;
}

/*
================
SubdivideTerrain

Recursively splits terrain into lightmap-sized chunks (max 512x512)
================
*/
#define TERRAIN_TEXELS_PER_SPLIT  472
#define MAX_TERRAIN_SPLITS        32
#define VERT_UNMAPPED             0xFFFF

int SubdivideTerrain(Entity_t *entity, float *verts, int vertCount, unsigned short *indexes, int triCount, Patch_t *surface)
{
  MeshVert_t *mv;
  TerrainNode_t *terrainNode;
  unsigned short *vertMap, *triOut;
  MeshVert_t *splitVerts;
  float minBounds[3], maxBounds[3], extents[3];
  float lmMin[2], lmMax[2];
  float splitStep;
  int lmTexelU, lmTexelV, maxTexel, axis;
  int splitCount, usedSplits, grid;
  int i, j, i0, i1, i2, n;
  double centroid;

  /* per-split: [0]=vertMap, [1]=idxBuf, [2]=vertBuf */
  unsigned short *splitVertMaps[MAX_TERRAIN_SPLITS];
  unsigned short *splitIdxBufs[MAX_TERRAIN_SPLITS];
  MeshVert_t     *splitVertBufs[MAX_TERRAIN_SPLITS];
  unsigned short  splitTriCounts[MAX_TERRAIN_SPLITS];
  unsigned short  splitVertCounts[MAX_TERRAIN_SPLITS];

  mv = (MeshVert_t *)verts;

  /* compute spatial and lightmap UV bounds */
  ClearBounds(minBounds, maxBounds);
  ClearBounds2D(lmMin, lmMax);
  for ( i = 0; i < vertCount; i++ )
  {
    AddPointToBounds(mv[i].pos, minBounds, maxBounds);
    AddPointToBounds2D(mv[i].lmSt, lmMin, lmMax);
  }

  /* expand bounds slightly */
  #define TERRAIN_BOUNDS_EXPAND 0.1f
  #define ONE_THIRD 0.33333334f
  for ( i = 0; i < 3; i++ )
  {
    minBounds[i] -= TERRAIN_BOUNDS_EXPAND;
    maxBounds[i] += TERRAIN_BOUNDS_EXPAND;
    extents[i] = maxBounds[i] - minBounds[i];
  }

  /* determine if splitting is needed */
  lmTexelU = LightmapTexelSize(lmMin[0], lmMax[0]);
  lmTexelV = LightmapTexelSize(lmMin[1], lmMax[1]);

  if ( lmTexelU <= LIGHTMAP_SIZE && lmTexelV <= LIGHTMAP_SIZE )
    splitCount = 0;
  else
  {
    axis = VecLargestAxis(extents);
    maxTexel = (lmTexelU > lmTexelV) ? lmTexelU : lmTexelV;
    splitCount = (int)ceil((double)maxTexel / TERRAIN_TEXELS_PER_SPLIT);
  }

  if ( splitCount )
  {
    /* validate split parameters */
    Assert(splitCount >= 2, s_assertDisable_SubdivideTerrain);

    splitStep = (float)((double)splitCount / extents[axis]);
    Assert(splitStep > 0.0f, s_assertDisable_SubdivideTerrain);

    if ( splitCount > MAX_TERRAIN_SPLITS )
      Com_Error("terrain mesh exceeded maximum lightmap splits (%i > %i)\n", splitCount, MAX_TERRAIN_SPLITS);

    /* allocate per-split buffers */
    memset(splitTriCounts, 0, sizeof(splitTriCounts));
    memset(splitVertCounts, 0, sizeof(splitVertCounts));
    for ( i = 0; i < splitCount; i++ )
    {
      splitIdxBufs[i] = malloc(3 * sizeof(unsigned short) * triCount);
      splitVertBufs[i] = malloc(sizeof(MeshVert_t) * vertCount);
      splitVertMaps[i] = malloc(sizeof(unsigned short) * vertCount);
      memset(splitVertMaps[i], 0xFF, sizeof(unsigned short) * vertCount);
    }

    /* distribute triangles into grid cells */
    usedSplits = 0;
    for ( i = 0; i < triCount; i++ )
    {
      i0 = indexes[3 * i];
      i1 = indexes[3 * i + 1];
      i2 = indexes[3 * i + 2];

      /* compute triangle centroid along split axis */
      centroid = (mv[i0].pos[axis] + mv[i1].pos[axis] + mv[i2].pos[axis]) * ONE_THIRD;
      grid = (int)floor((centroid - minBounds[axis]) * splitStep);

      Assert(grid >= 0 && grid < splitCount, s_assertDisable_SubdivideTerrain);

      Assert(!(i0 >= vertCount), s_assertDisable_SubdivideTerrain);
      Assert(!(i1 >= vertCount), s_assertDisable_SubdivideTerrain);
      Assert(!(i2 >= vertCount), s_assertDisable_SubdivideTerrain);

      /* remap each vertex into this cell's local buffer */
      vertMap = splitVertMaps[grid];
      splitVerts = splitVertBufs[grid];

      if ( vertMap[i0] == VERT_UNMAPPED )
      {
        n = splitVertCounts[grid];
        vertMap[i0] = n;
        splitVerts[n] = mv[i0];
        splitVertCounts[grid] = n + 1;
      }
      if ( vertMap[i1] == VERT_UNMAPPED )
      {
        n = splitVertCounts[grid];
        vertMap[i1] = n;
        splitVerts[n] = mv[i1];
        splitVertCounts[grid] = n + 1;
      }
      if ( vertMap[i2] == VERT_UNMAPPED )
      {
        n = splitVertCounts[grid];
        vertMap[i2] = n;
        splitVerts[n] = mv[i2];
        splitVertCounts[grid] = n + 1;
      }

      Assert(!(splitVertCounts[grid] > vertCount), g_assertFlags0);

      /* write remapped triangle */
      triOut = splitIdxBufs[grid] + 3 * splitTriCounts[grid];
      triOut[0] = vertMap[i0];
      triOut[1] = vertMap[i1];
      triOut[2] = vertMap[i2];
      if ( !splitTriCounts[grid] )
        ++usedSplits;
      splitTriCounts[grid]++;
    }

    /* recursively subdivide each cell, then free buffers */
    for ( j = 0; j < splitCount; j++ )
    {
      /* Native 0x429A80 recurses only into occupied split buckets. */
      if ( usedSplits > 1 && splitVertCounts[j] )
        SubdivideTerrain(entity, (float *)splitVertBufs[j], splitVertCounts[j], splitIdxBufs[j], splitTriCounts[j], surface);
      free(splitIdxBufs[j]);
      free(splitVertBufs[j]);
      free(splitVertMaps[j]);
    }
    if ( usedSplits > 1 )
      return splitCount;
  } /* end if (splitCount) */

  /* create terrain leaf node */
  terrainNode = malloc(sizeof(*terrainNode));
  terrainNode->next = entity->terrainList;
  entity->terrainList = terrainNode;
  terrainNode->shaderInfo = surface->material;
  terrainNode->materialName = (const char *)surface->lmMaterial;
  terrainNode->surfaceFlags = surface->surfaceFlags;
  terrainNode->contentFlags = surface->contentFlags;
  terrainNode->smoothing = surface->smoothing;
  terrainNode->outputNum = surface->outputNum;
  terrainNode->numVerts = vertCount;
  terrainNode->verts = malloc(sizeof(MeshVert_t) * vertCount);
  memcpy(terrainNode->verts, verts, sizeof(MeshVert_t) * vertCount);
  terrainNode->numIndexes = 3 * triCount;
  terrainNode->indexes = malloc(sizeof(unsigned short) * terrainNode->numIndexes);
  memcpy(terrainNode->indexes, indexes, sizeof(unsigned short) * terrainNode->numIndexes);
  if ( (terrainNode->shaderInfo->contentFlags & CONTENTS_MISSILECLIP) && !(terrainNode->shaderInfo->contentFlags & CONTENTS_TELEPORTER) )
    WriteDebugSurfaceToFile(terrainNode);
  DrawSurfaceForTerrain(terrainNode, surface);
  return 0;
  #undef TERRAIN_BOUNDS_EXPAND
  #undef ONE_THIRD
}

/*
================
EmitTerrainGroup

Converts terrain mesh group to triangles and subdivides for lightmaps
================
*/
void EmitTerrainGroup(Entity_t *entity, Patch_t **meshList, int meshCount)
{
  int totalTris, totalVerts;
  int numIndexes, numVerts;
  int w, h, i;
  void *indexBuf, *vertBuf;
  unsigned char stackIndexBuf[32768];
  unsigned char stackVertBuf[180224];

  /* count total vertices and triangles across all meshes */
  totalTris = 0;
  totalVerts = 0;
  for ( i = 0; i < meshCount; i++ )
  {
    Patch_t *mesh = meshList[i];
    w = mesh->width;
    h = mesh->height;
    totalVerts += w * h;
    totalTris += (w - 1) * (2 * h - 2);
  }

  /* allocate buffers (use stack for small meshes) */
  if ( 3 * totalTris > MAX_PATCH_INDEXES )
    indexBuf = malloc(sizeof(unsigned short) * 3 * totalTris);
  else
    indexBuf = stackIndexBuf;
  if ( totalVerts > MAX_PATCH_VERTS )
    vertBuf = malloc(sizeof(MeshVert_t) * totalVerts);
  else
    vertBuf = stackVertBuf;

  /* convert each mesh to triangles and subdivide */
  numIndexes = 0;
  numVerts = 0;
  for ( i = 0; i < meshCount; i++ )
  {
    Patch_t *mesh = meshList[i];
    MeshToTriangles(vertBuf, &numIndexes, mesh, &numVerts, indexBuf);
    SubdivideTerrain(entity, vertBuf, numVerts, indexBuf, numIndexes, mesh);
  }

  if ( indexBuf != stackIndexBuf )
    free(indexBuf);
  if ( vertBuf != stackVertBuf )
    free(vertBuf);
}

/*
================
ProcessEntityPatches

Groups terrain patches by material and emits each group
================
*/
void ProcessEntityPatches(Entity_t *entity)
{
  Patch_t *pm, *prev, *cur;
  Patch_t **arr;
  int terrainCount, patchCount, groupStart, groupSize;
  int assertResult, i;
  Patch_t *stackArr[MAX_TERRAIN_PATCHES];

  /* count terrain patches */
  terrainCount = 0;
  for ( pm = entity->patches; pm; pm = pm->next )
  {
    if ( pm->patchType )
      ++terrainCount;
  }
  if ( !terrainCount )
    return;

  Assert(terrainCount >= 0, g_assertFlags1);

  /* collect terrain patches into sortable array */
  if ( terrainCount > MAX_TERRAIN_PATCHES )
    arr = malloc(sizeof(Patch_t *) * terrainCount);
  else
    arr = stackArr;

  patchCount = 0;
  for ( pm = entity->patches; pm; pm = pm->next )
  {
    if ( pm->patchType )
      arr[patchCount++] = pm;
  }

  /* CoD4 0x429288 sorts the Patch_t pointer array directly with its CRT
     qsort before forming equal-key terrain groups.  Do not substitute a
     stable sort: equal material meshes retain the CRT's native swap history,
     which affects each emitted quad's starting vertex. */
  qsort(arr, patchCount, sizeof(*arr), ComparePatchSurfaces);

  /* emit groups of matching patches */
  groupStart = 0;
  groupSize = 1;
  for ( i = 1; i < patchCount; i++ )
  {
    prev = arr[i - 1];
    cur = arr[i];

    /* The native path calls its qsort comparator again for grouping. */
    if ( ComparePatchSurfaces(&prev, &cur) )
    {
      EmitTerrainGroup(entity, &arr[groupStart], groupSize);
      groupStart = i;
      groupSize = 1;
    }
    else
    {
      ++groupSize;
    }
  }
  EmitTerrainGroup(entity, &arr[groupStart], groupSize);

  if ( arr != stackArr )
    free(arr);
}

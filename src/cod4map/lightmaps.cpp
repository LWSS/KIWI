/*
lightmaps.c — Lightmap allocation, UV computation, baking

Reconstructed from cod2map.exe by Rose.
*/

#include "cod4map.h"

LmAllowedNode_t *lmAllowedList;
LmFreeBlock_t   *lmFreeBlocks;

/*
   Native lightmap grouping (0x43E80B -> 0x442F90) allocates once, then emits
   both draw-surface types from that single triangulation.  KIWI's donor
   emitter must reconstruct the source geometry for its second type, so retain
   an exact allocator snapshot across that reconstruction.  This is dormant
   unless the native route is explicitly enabled.
*/
static LmFreeBlock_t *s_trisLmapCheckpointBlocks;
static int s_trisLmapCheckpointNumLightmaps;

static void TrisLmap_FreeBlockList(LmFreeBlock_t *block)
{
  while ( block )
  {
    LmFreeBlock_t *next = block->next;
    free(block);
    block = next;
  }
}

static LmFreeBlock_t *TrisLmap_CloneBlockList(const LmFreeBlock_t *source)
{
  LmFreeBlock_t *head = NULL;
  LmFreeBlock_t *tail = NULL;

  while ( source )
  {
    LmFreeBlock_t *copy = (LmFreeBlock_t *)malloc(sizeof(*copy));
    if ( !copy )
      Com_Error("TrisLmap_CloneBlockList: out of memory");
    *copy = *source;
    copy->prev = tail;
    copy->next = NULL;
    if ( tail )
      tail->next = copy;
    else
      head = copy;
    tail = copy;
    source = source->next;
  }
  return head;
}

void TrisLmap_ClearAllocatorCheckpoint(void)
{
  TrisLmap_FreeBlockList(s_trisLmapCheckpointBlocks);
  s_trisLmapCheckpointBlocks = NULL;
  s_trisLmapCheckpointNumLightmaps = 0;
}

void TrisLmap_SaveAllocatorCheckpoint(void)
{
  TrisLmap_ClearAllocatorCheckpoint();
  s_trisLmapCheckpointBlocks = TrisLmap_CloneBlockList(lmFreeBlocks);
  s_trisLmapCheckpointNumLightmaps = numLightmaps;
}

void TrisLmap_RestoreAllocatorCheckpoint(void)
{
  TrisLmap_FreeBlockList(lmFreeBlocks);
  lmFreeBlocks = TrisLmap_CloneBlockList(s_trisLmapCheckpointBlocks);
  numLightmaps = s_trisLmapCheckpointNumLightmaps;
}

char   g_fmtTwoFloats[8] = { '%', 'g', ',', ' ', '%', 'g', '\0', '\0' };
int    leakFileWritten;
float  lightmapEfficiency;
size_t NumOfElements;

char s_assertDisable_AbsorbLightmapGroup;
char s_assertDisable_AllocateAndBakeGroupLightmap;
char s_assertDisable_AllocateAndBakeGroupLightmap;
char s_assertDisable_AllocateLightmaps;
char s_assertDisable_BuildLightmapGroups;
char s_assertDisable_BuildLightmapGroups;
char s_assertDisable_CheckVertexPairCompatibility;
char s_assertDisable_CheckVertexPairCompatibility;
char s_assertDisable_CheckVertexPairCompatibility;
char s_assertDisable_CheckVertexPairCompatibility;
char s_assertDisable_CheckVertexPairCompatibility;
char s_assertDisable_CheckVertexPairCompatibility;
char s_assertDisable_ComputeSurfaceLightmapBounds;
char s_assertDisable_MergeFreeBlocks;
char s_assertDisable_MergeSortLightmapSurfaces;
char s_assertDisable_PrepareGroupLightmapUVs;
char s_assertDisable_PrepareGroupLightmapUVs;
char s_assertDisable_PrepareGroupLightmapUVs;
char s_assertDisable_PrepareGroupLightmapUVs;
char s_assertDisable_TestSurfacesTouch;
char s_assertDisable_TestSurfacesTouch;
char s_assertDisable_TryMergeSurfaceGroups;
char s_assertDisable_TryMergeSurfaceGroups;
char s_assertDisable_ValidateLightmapGroups;
char s_assertDisable_ValidateLightmapGroups;
char s_assertDisable_DebugLightmapBlock;


/*
================
MergeFreeBlocks

Merges adjacent free blocks in lightmap allocator free list
================
*/
void MergeFreeBlocks(LmFreeBlock_t *block)
{
  LmFreeBlock_t *other, *toFree;
  int merged, result;

  /* repeatedly merge until no more adjacent blocks found */
  do
  {
    merged = 0;
    for ( other = lmFreeBlocks; other; other = other->next )
    {
      toFree = NULL;
      if ( block->lightmapIdx != other->lightmapIdx || block == other )
        continue;

      if ( block->y == other->y )
      {
        /* same row — assert not same position */
        Assert(!(block->x == other->x), s_assertDisable_MergeFreeBlocks);

        /* merge horizontally if same height */
        if ( block->height == other->height )
        {
          if ( block->x == other->x + other->width )
          {
            other->width += block->width;
            toFree = block;
          }
          else if ( block->x + block->width == other->x )
          {
            block->width += other->width;
            toFree = other;
          }
        }
      }
      else if ( block->x == other->x && block->width == other->width )
      {
        /* merge vertically if same width */
        if ( block->y == other->y + other->height )
        {
          other->height += block->height;
          toFree = block;
        }
        else if ( block->y + block->height == other->y )
        {
          block->height += other->height;
          toFree = other;
        }
      }

      if ( toFree )
      {
        /* unlink and free merged block */
        if ( toFree->prev )
          toFree->prev->next = toFree->next;
        if ( toFree->next )
          toFree->next->prev = toFree->prev;
        if ( lmFreeBlocks == toFree )
          lmFreeBlocks = toFree->next;
        free(toFree);
        if ( toFree == block )
          block = other;
        merged = 1;
        break;
      }
    }
  }
  while ( merged );
}

/*
================
SplitFreeBlock

Splits remainder of free block after allocation
================
*/
void SplitFreeBlock(int allocHeight, LmFreeBlock_t *freeBlock, int allocWidth)
{
  int blockHeight;
  int remainWidth;
  int remainHeight;
  int splitArea;
  int otherDim;
  LmFreeBlock_t *newBlock;
  int prevPtr2;

  blockHeight = freeBlock->height;
  if ( allocHeight == blockHeight )
  {
    remainWidth = freeBlock->width - allocWidth;
    freeBlock->x += allocWidth;
    freeBlock->width = remainWidth;
    #define LM_MIN_BLOCK_SIZE 2
    if ( remainWidth <= LM_MIN_BLOCK_SIZE )
    {
      if ( freeBlock->prev )
        freeBlock->prev->next = freeBlock->next;
      if ( freeBlock->next )
        freeBlock->next->prev = freeBlock->prev;
      if ( lmFreeBlocks == freeBlock )
        lmFreeBlocks = freeBlock->next;
      free(freeBlock);
      return;
    }
  }
  else if ( allocWidth != freeBlock->width )
  {
    prevPtr2 = freeBlock->width;
    splitArea = allocWidth * blockHeight;
    otherDim = blockHeight - allocHeight;
    if ( splitArea >= allocHeight * prevPtr2 )
    {
      freeBlock->y += allocHeight;
      freeBlock->height = otherDim;
      if ( prevPtr2 - allocWidth > LM_MIN_BLOCK_SIZE )
      {
        newBlock = malloc(sizeof(*newBlock));
        newBlock->y = freeBlock->y - allocHeight;
        newBlock->x = allocWidth + freeBlock->x;
        newBlock->height = allocHeight;
        newBlock->width = freeBlock->width - allocWidth;
        newBlock->lightmapIdx = freeBlock->lightmapIdx;
        newBlock->prev = freeBlock;
        newBlock->next = freeBlock->next;
        if ( freeBlock->next )
          freeBlock->next->prev = newBlock;
        freeBlock->next = newBlock;
        MergeFreeBlocks(newBlock);
      }
    }
    else
    {
      freeBlock->x += allocWidth;
      freeBlock->width = prevPtr2 - allocWidth;
      if ( otherDim > 2 )
      {
        newBlock = malloc(sizeof(*newBlock));
        newBlock->y = allocHeight + freeBlock->y;
        newBlock->x = freeBlock->x - allocWidth;
        newBlock->height = freeBlock->height - allocHeight;
        newBlock->width = allocWidth;
        newBlock->lightmapIdx = freeBlock->lightmapIdx;
        newBlock->prev = freeBlock;
        newBlock->next = freeBlock->next;
        if ( freeBlock->next )
          freeBlock->next->prev = newBlock;
        freeBlock->next = newBlock;
        MergeFreeBlocks(newBlock);
      }
    }
  }
  else
  {
    remainHeight = blockHeight - allocHeight;
    freeBlock->y += allocHeight;
    freeBlock->height = remainHeight;
    if ( remainHeight <= LM_MIN_BLOCK_SIZE )
    {
      if ( freeBlock->prev )
        freeBlock->prev->next = freeBlock->next;
      if ( freeBlock->next )
        freeBlock->next->prev = freeBlock->prev;
      if ( lmFreeBlocks == freeBlock )
        lmFreeBlocks = freeBlock->next;
      free(freeBlock);
      return;
    }
  }
  MergeFreeBlocks(freeBlock);
}

/* 0x418510: reclaimed rectangles return to the same allocator list used by
   0x4185E0.  The native allocator expects this list to already be non-empty. */
void FreeLMBlock(int lightmapIdx, int y, int x, int height, int width)
{
  LmFreeBlock_t *block;

  block = (LmFreeBlock_t *)malloc(sizeof(*block));
  if (!block)
    Com_Error("FreeLMBlock: out of memory");
  block->lightmapIdx = lightmapIdx;
  block->y = y;
  block->x = x;
  block->height = height;
  block->width = width;
  block->prev = NULL;
  block->next = lmFreeBlocks;
  Assert(lmFreeBlocks, s_assertDisable_MergeFreeBlocks);
  lmFreeBlocks->prev = block;
  lmFreeBlocks = block;
}

/*
================
FindFreeBlock

Finds best-fit free block for requested dimensions in lightmap allocator
================
*/
int FindFreeBlock(int reqWidth, int reqHeight, int *outLightmap, int *outY, int *outX, int *outRotated, LmAllowedNode_t *allowedLmapList)
{
  LmFreeBlock_t *cur, *best;
  int ew, eh, waste, area;
  int bestWaste, savedBestWaste;
  int bestMinExcess, bestPartialArea;
  int rotated, savedPass;
  int found, pass;
  int origW, origH;

  origW = reqWidth;
  origH = reqHeight;
  best = NULL;
  bestWaste = 0;
  savedBestWaste = 0;
  bestMinExcess = 0;
  bestPartialArea = 0;
  rotated = 0;
  savedPass = 0;

  found = 0;
  for ( pass = 0; pass < 2; pass++ )
  {
    for ( cur = lmFreeBlocks; cur; cur = cur->next )
    {
      if ( allowedLmapList )
      {
        /* CoD4's native loop (0x41871C..0x418726) walks every allowed
           lightmap index before considering a free rectangle. */
        LmAllowedNode_t *allowed;

        for ( allowed = allowedLmapList;
              allowed && cur->lightmapIdx != allowed->lmapIdx;
              allowed = allowed->next )
        {
        }

        if ( !allowed )
          continue;
      }

      /* exact fit */
      if ( cur->height == reqWidth && cur->width == reqHeight )
      {
        found = 1;
        break;
      }

      /* height matches, width larger (1D excess on width) */
      if ( cur->height == reqWidth && cur->width > reqHeight )
      {
        waste = cur->width - reqHeight;
        if ( !best || bestMinExcess > 0 || bestWaste < waste )
        {
          bestWaste = waste;
          savedBestWaste = waste;
          best = cur;
          bestMinExcess = 0;
          rotated = pass == 1;
        }
      }
	  
      /* width matches, height larger (1D excess on height) */
      else if ( cur->width == reqHeight && cur->height > reqWidth )
      {
        waste = cur->height - reqWidth;
		
        /* 1D always beats 2D; among 1D, prefer larger waste */
        if ( !best || bestMinExcess > 0 || bestWaste < waste )
        {
          bestWaste = waste;
          savedBestWaste = waste;
          best = cur;
          bestMinExcess = 0;
          rotated = pass == 1;
        }
      }
	  
      /* both larger — 2D waste */
      else if ( cur->height > reqWidth && cur->width > reqHeight )
      {
        ew = cur->height - reqWidth;
        eh = cur->width - reqHeight;
        area = ew * cur->width;
        if ( area >= eh * cur->height )
          area = eh * cur->height;

        if ( best && (bestMinExcess <= 0 || bestPartialArea <= area) )
        {
          bestWaste = savedBestWaste;
        }
        else
        {
          int minEx = (ew < eh) ? ew : eh;
          best = cur;
          bestMinExcess = minEx;
          bestWaste = ew + eh - minEx;
          savedBestWaste = bestWaste;
          bestPartialArea = area;
          rotated = savedPass == 1;
        }
      }
    }

    if ( found )
    {
      *outLightmap = cur->lightmapIdx;
      *outY = cur->y;
      *outX = cur->x;
      *outRotated = pass == 1;
      if ( cur->prev ) cur->prev->next = cur->next;
      if ( cur->next ) cur->next->prev = cur->prev;
      if ( lmFreeBlocks == cur ) lmFreeBlocks = cur->next;
      free(cur);
      return 1;
    }

    if ( reqWidth == reqHeight )
      break;

    { int tmp = reqWidth; reqWidth = reqHeight; reqHeight = tmp; }
    savedPass = 1;
  }

  if ( !best )
    return 0;

  *outLightmap = best->lightmapIdx;
  *outY = best->y;
  *outX = best->x;
  *outRotated = rotated;

  if ( rotated )
    SplitFreeBlock(origH, best, origW);
  else
    SplitFreeBlock(origW, best, origH);
  return 1;
}

/*
================
AllocNewLightmap

Allocates new 512x512 lightmap and adds to free-block list
================
*/
void AllocNewLightmap()
{
  LmFreeBlock_t *newBlock;

  /* create full-size free block for new lightmap */
  newBlock = malloc(sizeof(*newBlock));
  newBlock->lightmapIdx = numLightmaps;
  /* Native CoD4 reserves atlas column zero.  0x418D80 initializes the
     first free rectangle as y=0, x=1, height=512, width=511; this is
     observable in baked lightmap V coordinates. */
  newBlock->height = LIGHTMAP_SIZE;
  newBlock->width = LIGHTMAP_SIZE - 1;
  newBlock->y = 0;
  newBlock->x = 1;

  /* prepend to free block list */
  newBlock->next = lmFreeBlocks;
  newBlock->prev = NULL;
  if ( lmFreeBlocks )
    lmFreeBlocks->prev = newBlock;
  lmFreeBlocks = newBlock;

  numLightmaps++;
  if ( numLightmaps * MAX_LIGHTMAP_BYTES > MAX_MAP_LIGHTBYTES )
    Com_Error("MAX_MAP_LIGHTBYTES (%g MB) exceeded... more than %i lightmaps used\n",
              (double)MAX_MAP_LIGHTMAPS, numLightmaps - 1);
}

/* CoD4 0x418E60: paint both atlas layers with a randomized checkerboard for
   the -debugLightmaps diagnostic route. */
static void DebugLightmapBlock(int lightmapIdx, int y, int x, int width, int height, int rotated)
{
  unsigned char colors[2][4];
  int endX = x + (rotated ? height : width);
  int endY = y + (rotated ? width : height);
  int row;
  int column;

  colors[0][0] = (unsigned char)((rand() << 7) / 0x8000 + 64);
  colors[0][1] = (unsigned char)((rand() << 7) / 0x8000 + 64);
  colors[0][2] = (unsigned char)((rand() << 7) / 0x8000 + 64);
  colors[0][3] = 128;
  colors[1][0] = (unsigned char)(4 * colors[0][0] / 5);
  colors[1][1] = (unsigned char)(4 * colors[0][1] / 5);
  colors[1][2] = (unsigned char)(4 * colors[0][2] / 5);
  colors[1][3] = 128;

  for ( row = y; row < endY; ++row )
  {
    for ( column = x; column < endX; ++column )
    {
      unsigned char *pixel = (unsigned char *)bspLightmapData
        + (size_t)lightmapIdx * LIGHTMAP_BYTES + 2048 * row + 4 * column;
      unsigned char *secondaryPixel = pixel + 0x100000;
      const unsigned char *color = colors[(row + column) & 1];

      Assert(pixel[0] == 0, s_assertDisable_DebugLightmapBlock);
      memcpy(pixel, color, sizeof(colors[0]));
      Assert(secondaryPixel[0] == 0, s_assertDisable_DebugLightmapBlock);
      memcpy(secondaryPixel, color, sizeof(colors[0]));
    }
  }
}

/*
================
AllocLMBlock

Allocates a block in a lightmap texture, creating new lightmap if needed
================
*/
int AllocLMBlock(int w, int h, int *outLightmap, int *outY, int *outX, int *outRotated)
{
  /* try existing lightmaps: with allowed list first, then any */
  if ( !FindFreeBlock(w, h, outLightmap, outY, outX, outRotated, lmAllowedList)
    && (!lmAllowedList || !FindFreeBlock(w, h, outLightmap, outY, outX, outRotated, NULL)) )
  {
    AllocNewLightmap();
    if ( !FindFreeBlock(w, h, outLightmap, outY, outX, outRotated, NULL) )
      return 0;
  }

  /* no space — allocate new lightmap and retry */
  if ( debugLightmaps )
    DebugLightmapBlock(*outLightmap, *outY, *outX, w, h, *outRotated);
  return 1;
}

/*
================
LightmapTexelSize

Computes lightmap texel span from min/max UV coordinates
================
*/
int LightmapTexelSize(float minUV, float maxUV)
{
  volatile float minTexel;
  volatile float maxTexel;

  /* Match the native helpers at 0x4190F0 / 0x419120 / 0x419150:
     x87 stores the scaled value to float before floor/ceil.  Keeping that
     store is significant for bounds which sit on a lightmap texel edge. */
  minTexel = minUV * (float)LIGHTMAP_SIZE - 0.5f;
  maxTexel = maxUV * (float)LIGHTMAP_SIZE + 0.5f;
  return (int)ceil(maxTexel) + 1 - (int)floor(minTexel);
}

/*
================
PrepareGroupLightmapUVs

Computes texel bounds and sets lightmap dimensions for surface group
================
*/
int PrepareGroupLightmapUVs(LightmapGroup_t *group)
{
  DrawSurf_t *surf;
  int texelMinU, texelMinV;
  int lightmapWidth, lightmapHeight;
  int i;
  float maxs[2];
  float mins[2];
  volatile float nativeMinTexelU;
  volatile float nativeMinTexelV;

  /* compute UV bounds across all surfaces in group */
  ClearBounds2D(mins, maxs);
  for ( surf = group->firstSurf; surf; surf = surf->nextInGroup )
    for ( i = 0; i < surf->numVerts; i++ )
      AddPointToBounds2D(surf->verts[i].lmSt, mins, maxs);

  /* compute lightmap pixel dimensions from UV bounds */
  nativeMinTexelU = mins[0] * (float)LIGHTMAP_SIZE - 0.5f;
  texelMinU = (int)floor(nativeMinTexelU);
  lightmapWidth = LightmapTexelSize(mins[0], maxs[0]);
  nativeMinTexelV = mins[1] * (float)LIGHTMAP_SIZE - 0.5f;
  texelMinV = (int)floor(nativeMinTexelV);
  lightmapHeight = LightmapTexelSize(mins[1], maxs[1]);
  Assert(lightmapWidth > 0, s_assertDisable_PrepareGroupLightmapUVs);
  Assert(lightmapHeight > 0, s_assertDisable_PrepareGroupLightmapUVs);
  
  /* compute UV origin (snapped to texel grid) */
  { float uvOriginU = (float)texelMinU * (1.0f / LIGHTMAP_SIZE);
  float uvOriginV = (float)texelMinV * (1.0f / LIGHTMAP_SIZE);

  /* offset each surface's UVs relative to group origin */
  for ( surf = group->firstSurf; surf; surf = surf->nextInGroup )
  {
    Assert(!surf->lightmapWidth  || s_assertDisable_PrepareGroupLightmapUVs, s_assertDisable_PrepareGroupLightmapUVs);
    Assert(!surf->lightmapHeight || s_assertDisable_PrepareGroupLightmapUVs, s_assertDisable_PrepareGroupLightmapUVs);

    surf->lightmapWidth = lightmapWidth;
    surf->lightmapHeight = lightmapHeight;
    for ( i = 0; i < surf->numVerts; i++ )
    {
      MeshVert_t *mv = &surf->verts[i];
      mv->lmSt[0] -= uvOriginU;
      mv->lmSt[1] -= uvOriginV;
    }
  }
  }
  return 0;
}

/*
================
AllocateAndBakeGroupLightmap

Allocates lightmap block and bakes UV coordinates into vertices
================
*/
int AllocateAndBakeGroupLightmap(LightmapGroup_t *group)
{
  DrawSurf_t *surf;
  int w, h, allocW, allocH;
  int isRotated;
  int uAxis, vAxis; /* UV axis mapping: 0 or 1, swapped when rotated */
  int i;
  float bakedUV[2];
  
  /* volatile: force 32-bit float reload each iteration for FPU precision matching */
  volatile float blockOriginUV[2];
  volatile float scaleU;
  volatile float scaleV;
  int lmapIdxOut, allocYOut, allocXOut, rotatedOut;

  surf = group->firstSurf;

  /* clamp lightmap dimensions to LIGHTMAP_SIZE */
  w = surf->lightmapWidth;
  allocW = (w <= LIGHTMAP_SIZE) ? w : LIGHTMAP_SIZE;
  scaleU = (w <= LIGHTMAP_SIZE) ? 1.0f : (float)LIGHTMAP_SIZE / w;

  h = surf->lightmapHeight;
  allocH = (h <= LIGHTMAP_SIZE) ? h : LIGHTMAP_SIZE;
  scaleV = (h <= LIGHTMAP_SIZE) ? 1.0f : (float)LIGHTMAP_SIZE / h;

  g_numMapBrushSides += allocW * allocH;

  if ( !AllocLMBlock(allocW, allocH, &lmapIdxOut, &allocYOut, &allocXOut, &rotatedOut) )
    Com_Error("Map %s, Entity %i, brush %i: Lightmap allocation failed", surf->mapName, surf->entityNum, surf->brushNum);

  /* swap dimensions if allocation is rotated */
  if ( rotatedOut )
  {
    int tmp = allocW; allocW = allocH; allocH = tmp;
    isRotated = 1;
    uAxis = 1; vAxis = 0; /* UV axes swapped */
  }
  else
  {
    isRotated = 0;
    uAxis = 0; vAxis = 1; /* UV axes normal */
  }

  /* compute UV origin from allocation position */
  blockOriginUV[0] = (float)allocYOut * (1.0f / LIGHTMAP_SIZE);
  blockOriginUV[1] = (float)allocXOut * (1.0f / LIGHTMAP_SIZE);

  /* bake lightmap UVs into each surface's vertices */
  for ( ; surf; surf = surf->nextInGroup )
  {
    surf->lightmapWidth = allocW;
    surf->lightmapHeight = allocH;
    surf->lightStyle = lmapIdxOut;
    surf->lightmapAllocY = allocYOut;
    surf->lightmapAllocX = allocXOut;

    for ( i = 0; i < surf->numVerts; i++ )
    {
      MeshVert_t *mv = &surf->verts[i];

      /* scale and offset UVs, swapping axes if rotated */
      bakedUV[uAxis] = FMA1(blockOriginUV[uAxis], scaleU, mv->lmSt[0]);
      bakedUV[vAxis] = FMA1(blockOriginUV[vAxis], scaleV, mv->lmSt[1]);
      mv->lmSt[0] = bakedUV[0];
      mv->lmSt[1] = bakedUV[1];

      Assert((mv->lmSt[0] >= 0.0f && mv->lmSt[0] <= 1.0f) || s_assertDisable_AllocateAndBakeGroupLightmap, s_assertDisable_AllocateAndBakeGroupLightmap);
      Assert((mv->lmSt[1] >= 0.0f && mv->lmSt[1] <= 1.0f) || s_assertDisable_AllocateAndBakeGroupLightmap, s_assertDisable_AllocateAndBakeGroupLightmap);
    }
  }
  return 0;
}

/*
================
CollectTerrainVertsInBounds

Collects terrain vertices within bounding box
================
*/
int CollectTerrainVertsInBounds(DrawSurf_t *ds, float *mins, float *maxs, MeshVert_t **dvList)
{
  MeshVert_t *verts = ds->verts;
  int dvCount = 0;
  int row, col, idx;

  for ( row = 0; row < ds->patchHeight; row += 2 )
  {
    for ( col = 0; col < ds->patchWidth; col += 2 )
    {
      idx = col + row * ds->patchWidth;
      if ( PointInBounds(verts[idx].pos, mins, maxs) )
        dvList[dvCount++] = &verts[idx];
    }
  }
  return dvCount;
}

/*
================
CollectNormalVertsInBounds

Collects normal vertices within bounding box
================
*/
int CollectNormalVertsInBounds(int vertCount, MeshVert_t *verts, float *mins, float *maxs, MeshVert_t **dvList)
{
  int dvCount = 0;
  int i;

  for ( i = 0; i < vertCount; i++ )
  {
    if ( PointInBounds(verts[i].pos, mins, maxs) )
      dvList[dvCount++] = &verts[i];
  }
  return dvCount;
}

/*
================
CheckVertexPairCompatibility

Tests vertex pair compatibility for lightmap merging
================
*/
int CheckVertexPairCompatibility(MeshVert_t **dvList1, int ptCount0, MeshVert_t **dvList0, int ptCount1, float dotThreshold, unsigned int touchType, float *lmapShift)
{
  int dv1Idx, outerIdx;
  MeshVert_t *mv0, *mv1;
  float diff_UV[2];
  volatile float tempFloat, tempFloat2; /* force float store/reload before rounding */
  int shiftTexelU, shiftTexelV;

  Assert(ptCount0 >= 0, s_assertDisable_CheckVertexPairCompatibility);
  Assert(ptCount1 >= 0, s_assertDisable_CheckVertexPairCompatibility);
  Assert(dvList0, s_assertDisable_CheckVertexPairCompatibility);
  Assert(dvList1, s_assertDisable_CheckVertexPairCompatibility);
  Assert(touchType < 2, s_assertDisable_CheckVertexPairCompatibility);
  Assert(lmapShift, s_assertDisable_CheckVertexPairCompatibility);
  if ( ptCount0 <= 0 )
    return touchType;
  for ( outerIdx = 0; outerIdx < ptCount0; ++outerIdx )
  {
    for ( dv1Idx = 0; dv1Idx < ptCount1; ++dv1Idx )
    {
      mv0 = dvList0[outerIdx];
      mv1 = dvList1[dv1Idx];

      #define VERT_MATCH_EPSILON    0.001f
      #define LMAP_UV_EPSILON       0.00001953125f  /* 1.0 / (512 * 256) */

      /* check if vertices are at the same position */
      if ( VectorCompareEpsilon(mv0->pos, mv1->pos, VERT_MATCH_EPSILON, 3) )
      {
        /* normals must be compatible */
        if ( DotProduct210(mv0->normal, mv1->normal) < dotThreshold )
          return 2;

        /* compute lightmap UV difference */
        diff_UV[0] = mv0->lmSt[0] - mv1->lmSt[0];
        diff_UV[1] = mv0->lmSt[1] - mv1->lmSt[1];

        if ( touchType )
        {
          
        }
        else
        {
          /* first match — compute texel-snapped UV shift */
          touchType = 1;
          tempFloat = diff_UV[0] * LIGHTMAP_SIZE;
          shiftTexelU = xs_RoundToInt(tempFloat + FISTP_BIAS);
          
          *lmapShift = (double)shiftTexelU * (1.0 / LIGHTMAP_SIZE);
          tempFloat2 = diff_UV[1] * LIGHTMAP_SIZE;
          shiftTexelV = xs_RoundToInt(tempFloat2 + FISTP_BIAS);
          lmapShift[1] = (double)shiftTexelV * (1.0 / LIGHTMAP_SIZE);
        }

        /* UV shift must match across all vertex pairs */
        if ( !VectorCompareEpsilon(diff_UV, lmapShift, LMAP_UV_EPSILON, 2) )
          return 2;
      }
    }
  }
  return touchType;
}

/*
================
TestSurfacesTouch

Tests if two surfaces touch using vertex collection and compatibility check
================
*/
int TestSurfacesTouch(DrawSurf_t *ds0, DrawSurf_t *ds1, float dotThreshold, unsigned int touchType, float *lmapShift)
{
  int ptCount0, ptCount1;
  #define MAX_TOUCH_VERTS 65536
  MeshVert_t *dvList0[MAX_TOUCH_VERTS];
  MeshVert_t *dvList1[MAX_TOUCH_VERTS];

  Assert(ds0->numVerts <= MAX_TOUCH_VERTS, s_assertDisable_TestSurfacesTouch);
  Assert(ds1->numVerts <= MAX_TOUCH_VERTS, s_assertDisable_TestSurfacesTouch);
  
  /* collect vertices from ds0 within ds1's bounds */
  if ( ds0->isPatch )
    ptCount0 = CollectTerrainVertsInBounds(ds0, ds1->lmapMins, ds1->lmapMaxs, dvList0);
  else
    ptCount0 = CollectNormalVertsInBounds(ds0->numVerts, (MeshVert_t *)ds0->verts, ds1->lmapMins, ds1->lmapMaxs, dvList0);

  /* collect vertices from ds1 within ds0's bounds */
  if ( ds1->isPatch )
    ptCount1 = CollectTerrainVertsInBounds(ds1, ds0->lmapMins, ds0->lmapMaxs, dvList1);
  else
    ptCount1 = CollectNormalVertsInBounds(ds1->numVerts, (MeshVert_t *)ds1->verts, ds0->lmapMins, ds0->lmapMaxs, dvList1);

  return CheckVertexPairCompatibility(dvList1, ptCount0, dvList0, ptCount1, dotThreshold, touchType, lmapShift);
}

/*
================
TestTouchingAgainstGroupList

Tests surface touching against all surfaces in a group
================
*/
int TestTouchingAgainstGroupList(unsigned int touchType, float *lmapShift, DrawSurf_t *ds, LightmapGroup_t *group)
{
  DrawSurf_t *surf;
  float dot0, dot1, maxDot;

  /* get smooth angle threshold for source surface */
  if ( ds->isPatch )
    dot0 = smoothAngle - PLANESIDE_EPSILON;
  else if ( ds->isTerrain )
    dot0 = smoothAngle - PLANESIDE_EPSILON;
  else
    dot0 = smoothAngle - PLANESIDE_EPSILON;

  /* test against each surface in the group */
  for ( surf = group->firstSurf; surf; surf = surf->nextInGroup )
  {
    if ( surf->isPatch )
      dot1 = smoothAngle - PLANESIDE_EPSILON;
    else
      dot1 = smoothAngle - PLANESIDE_EPSILON;

    maxDot = (dot0 >= dot1) ? dot0 : dot1;
    touchType = TestSurfacesTouch(ds, surf, maxDot, touchType, lmapShift);
    if ( touchType == 2 )
      return 2;
  }
  return touchType;
}

/*
================
TestTouchingAcrossGroup

Tests surface touching across all surfaces in a group and its siblings
================
*/
unsigned int TestTouchingAcrossGroup(float *lmapShift, LightmapGroup_t *group, DrawSurf_t *ds)
{
  unsigned int result;

  result = TestTouchingAgainstGroupList(0, lmapShift, ds, group);

  if ( result == 1 )
  {
    LightmapGroup_t *grp = ds->ownerGroup;
    for ( DrawSurf_t *i = grp->firstSurf; i; i = i->nextInGroup )
    {
      if ( i != ds )
      {
        result = TestTouchingAgainstGroupList(result, lmapShift, i, group);
        if ( result != 1 )
          break;
      }
    }
  }
  return result;
}
/*
================
ComputeSurfaceLightmapBounds

Computes bounds of surface vertices with epsilon padding
================
*/
int ComputeSurfaceLightmapBounds(DrawSurf_t *surf)
{
  MeshVert_t *verts = surf->verts;
  int i, r;

  ClearBounds(surf->lmapMins, surf->lmapMaxs);

  Assert(!(surf->isPatch && surf->numVerts != surf->patchWidth * surf->patchHeight), s_assertDisable_ComputeSurfaceLightmapBounds);

  /* compute bounds from all vertex positions */
  for ( i = 0; i < surf->numVerts; i++ )
    AddPointToBounds(verts[i].pos, surf->lmapMins, surf->lmapMaxs);

  /* pad bounds by epsilon */
  surf->lmapMins[0] -= ON_EPSILON;
  surf->lmapMins[1] -= ON_EPSILON;
  surf->lmapMins[2] -= ON_EPSILON;
  surf->lmapMaxs[0] += ON_EPSILON;
  surf->lmapMaxs[1] += ON_EPSILON;
  surf->lmapMaxs[2] += ON_EPSILON;
  return surf->numVerts;
}

/* Native 0x451980 expands each group's UV bounds by this amount before group
   compatibility is considered.  0x452420 then rejects a merged group whose
   padded span reaches 512 texels on either axis. */
#define NATIVE_LMAP_GROUP_EPSILON 0.00007324219f

static void AddLightmapGroupUvBounds(LightmapGroup_t *group, float *mins, float *maxs)
{
  DrawSurf_t *surf;
  int vertIndex;

  for ( surf = group->firstSurf; surf; surf = surf->nextInGroup )
  {
    for ( vertIndex = 0; vertIndex < surf->numVerts; ++vertIndex )
      AddPointToBounds2D(surf->verts[vertIndex].lmSt, mins, maxs);
  }
}

/*
================
CanMergeLightmapGroups

The native group absorber (0x452420) computes prospective UV bounds and
declines the merge once either native-sized span is >= LMAP_WIDTH_MIN (512).
The allocator's new block is 512x511, so admitting a 512x512 group here
creates a request that cannot fit any new atlas.
================
*/
static int CanMergeLightmapGroups(LightmapGroup_t *dst, LightmapGroup_t *src)
{
  float mins[2];
  float maxs[2];

  ClearBounds2D(mins, maxs);
  AddLightmapGroupUvBounds(dst, mins, maxs);
  AddLightmapGroupUvBounds(src, mins, maxs);

  mins[0] -= NATIVE_LMAP_GROUP_EPSILON;
  mins[1] -= NATIVE_LMAP_GROUP_EPSILON;
  maxs[0] += NATIVE_LMAP_GROUP_EPSILON;
  maxs[1] += NATIVE_LMAP_GROUP_EPSILON;

  return LightmapTexelSize(mins[0], maxs[0]) < LIGHTMAP_SIZE
      && LightmapTexelSize(mins[1], maxs[1]) < LIGHTMAP_SIZE;
}

/*
================
AddNewLightmapGroup

Creates a new lightmap group for a surface
================
*/
LightmapGroup_t *AddNewLightmapGroup(LightmapGroup_t *prevGroup, DrawSurf_t *ds)
{
  LightmapGroup_t *grp;

  grp = malloc(sizeof(*grp));
  if ( !grp )
    Com_Error("AddNewLightmapGroup: Out of memory\n");

  ds->ownerGroup = grp;
  grp->firstSurf = ds;
  VectorCopy(ds->lmapMins, grp->lmapMins);
  VectorCopy(ds->lmapMaxs, grp->lmapMaxs);
  grp->prevGroup = NULL;
  grp->nextGroup = prevGroup;
  if ( prevGroup )
    prevGroup->prevGroup = grp;
  return grp;
}

/*
================
AbsorbLightmapGroup

Merges source group into destination group
================
*/
void AbsorbLightmapGroup(LightmapGroup_t *dst, LightmapGroup_t *src)
{
  DrawSurf_t *surf;

  Assert(src->prevGroup, s_assertDisable_AbsorbLightmapGroup);

  /* unlink source from group chain */
  src->prevGroup->nextGroup = src->nextGroup;
  if ( src->nextGroup )
    src->nextGroup->prevGroup = src->prevGroup;

  /* reassign all source surfaces to destination group */
  for ( surf = src->firstSurf; surf->nextInGroup; surf = surf->nextInGroup )
    surf->ownerGroup = dst;
  surf->ownerGroup = dst;

  /* splice source surface list onto front of destination */
  surf->nextInGroup = dst->firstSurf;
  dst->firstSurf = src->firstSurf;
  AddBoundsToBounds(src->lmapMins, src->lmapMaxs, dst->lmapMins, dst->lmapMaxs);
  free(src);
}

/*
================
TryMergeSurfaceGroups

Tries to merge a surface into compatible lightmap groups
================
*/
LightmapGroup_t *TryMergeSurfaceGroups(DrawSurf_t *ds, LightmapGroup_t *groupList)
{
  LightmapGroup_t *grp, *next, *head;
  float lmapShift[2];

  Assert(!(ds->ownerGroup), s_assertDisable_TryMergeSurfaceGroups);
  Assert(!(ds->nextInGroup), s_assertDisable_TryMergeSurfaceGroups);

  ComputeSurfaceLightmapBounds(ds);
  head = AddNewLightmapGroup(groupList, ds);
  if ( !head )
    return NULL;

  /* try to absorb compatible groups */
  for ( grp = head; grp; grp = next )
  {
    next = grp->nextGroup;
    if ( grp == ds->ownerGroup )
      continue;
    if ( !BoundsIntersect(ds->lmapMins, ds->lmapMaxs, grp->lmapMins, grp->lmapMaxs) )
      continue;
    if ( ds->materialName != grp->firstSurf->materialName )
      continue;
    if ( TestTouchingAcrossGroup(lmapShift, grp, ds) == 1
      && CanMergeLightmapGroups(ds->ownerGroup, grp) )
      AbsorbLightmapGroup(ds->ownerGroup, grp);
  }
  return head;
}

/*
================
BuildLightmapGroups

Builds lightmap groups by merging compatible surfaces
================
*/
LightmapGroup_t *BuildLightmapGroups(Entity_t *entity)
{
  DrawSurf_t *surf;
  LightmapGroup_t *groupList;
  int i, r;

  groupList = NULL;

  for ( i = entity->firstDrawSurf; i < numMapDrawSurfs; i++ )
  {
    surf = &g_drawSurfs[i];

    Assert(!(surf->ownerGroup), s_assertDisable_BuildLightmapGroups);
    Assert(!(surf->nextInGroup), s_assertDisable_BuildLightmapGroups);

    if ( surf->numVerts && (surf->shaderInfo->gameFlags & 2) )
      groupList = TryMergeSurfaceGroups(surf, groupList);
  }
  return groupList;
}

/*
================
ValidateLightmapGroups

Validates all groups have positive lightmap dimensions
================
*/
void ValidateLightmapGroups(LightmapGroup_t *groupList)
{
  LightmapGroup_t *grp;

  for ( grp = groupList; grp; grp = grp->nextGroup )
  {
    PrepareGroupLightmapUVs(grp);

    AssertFatal(grp->firstSurf->lightmapWidth > 0, s_assertDisable_ValidateLightmapGroups);
    AssertFatal(grp->firstSurf->lightmapHeight > 0, s_assertDisable_ValidateLightmapGroups);
  }
}

/*
================
MergeSortLightmapSurfaces

Merge-sorts surface list by lightmap area (largest first)
================
*/
DrawSurf_t *MergeSortLightmapSurfaces(DrawSurf_t *head, int count)
{
  int leftCount, rightCount, i;
  DrawSurf_t *left, *right, *split;
  DrawSurf_t **tail;

  Assert(count > 0 || s_assertDisable_MergeSortLightmapSurfaces, s_assertDisable_MergeSortLightmapSurfaces);

  if ( count <= 1 )
    return head;

  /* find midpoint */
  leftCount = count / 2;
  rightCount = count - leftCount;
  split = head;
  for ( i = 0; i < leftCount; i++ )
    split = split->matListNext;

  /* sort each half */
  left = MergeSortLightmapSurfaces(head, leftCount);
  right = MergeSortLightmapSurfaces(split, rightCount);

  /* merge by lightmap area (largest first) */
  tail = &head;
  while ( leftCount && rightCount )
  {
    if ( right->lightmapWidth * right->lightmapHeight > left->lightmapWidth * left->lightmapHeight )
    {
      *tail = right;
      right = right->matListNext;
      rightCount--;
    }
    else
    {
      *tail = left;
      left = left->matListNext;
      leftCount--;
    }
    tail = &(*tail)->matListNext;
  }
  
  /* append remaining */
  while ( rightCount-- > 0 )
  {
    *tail = right;
    right = (DrawSurf_t *)right->matListNext;
    tail = &(*tail)->matListNext;
  }
  while ( leftCount-- > 0 )
  {
    *tail = left;
    left = (DrawSurf_t *)left->matListNext;
    tail = &(*tail)->matListNext;
  }
  *tail = NULL;
  return head;
}

/*
================
CompareMaterialGroupsByArea

qsort comparator: sorts material groups by max area (descending)
================
*/
int CompareMaterialGroupsByArea(const void *va, const void *vb)
{
  const MatSortEntry_t *a = va;
  const MatSortEntry_t *b = vb;
  int diff;

  diff = b->maxArea - a->maxArea;
  if ( !diff )
    return b->count - a->count;
  return diff;
}

/*
================
AllocateLightmaps

Main entry: sorts surfaces, builds groups, allocates lightmap blocks, bakes UVs
================
*/
int AllocateLightmaps(Entity_t *entity)
{
  MatSortEntry_t *mat;
  DrawSurf_t *surf;
  ShaderInfo_t *si;
  LmAllowedNode_t *allowed, *node;
  LmFreeBlock_t *block;
  LightmapGroup_t *groups;
  int numMats, i, area, lmapCount, totalLmaps, wastedTexels;
  double efficiency;

  /* CoD4's native path allocates from 0x4540B0 during TriangulateEntity
     (0x43EB87), after this donor stage would otherwise have consumed the
     shared 0x4185E0 allocator.  Preserve only the donor draw-surface reset;
     the native-route assignments are carried by TriSurfProps sidecars. */
  if ( TrisLmap_NativeRouteEnabled() )
  {
    /* The donor executable initializes this to one for its legacy allocator.
       Native 0x418D80 uses the empty allocator's zero-based atlas count. */
    if ( !lmFreeBlocks )
      numLightmaps = 0;
    for ( i = entity->firstDrawSurf; i < numMapDrawSurfs; ++i )
    {
      surf = &g_drawSurfs[i];
      surf->lightStyle = LIGHTSTYLE_NONE;
      surf->ownerGroup = NULL;
      surf->nextInGroup = NULL;
    }
    return 0;
  }

  Com_DPrintf("--- AllocateLightmaps---\n");
  numMats = 0;
  NumOfElements = 0;

  /* build material groups from entity's draw surfaces */
  for ( i = entity->firstDrawSurf; i < numMapDrawSurfs; i++ )
  {
    surf = &g_drawSurfs[i];
    surf->lightStyle = LIGHTSTYLE_NONE;
    surf->ownerGroup = 0;
    surf->nextInGroup = 0;

    if ( !surf->numVerts )
      continue;

    /* find matching material group */
    for ( mat = materialGroupSortTable; mat < &materialGroupSortTable[numMats]; mat++ )
    {
      if ( surf->shaderInfo == mat->head->shaderInfo )
        break;
    }

    if ( mat < &materialGroupSortTable[numMats] )
    {
      /* prepend surface to existing material group */
      surf->matListNext = mat->head;
      mat->head = surf;
      mat->count++;
    }
    else
    {
      /* create new material group */
      if ( numMats >= MAX_MAP_MATERIALS )
      {
        Com_Error("MAX_MAP_MATERIALS (%i) exceeded", numMats);
        numMats = (int)NumOfElements;
      }
      mat->head = surf;
      mat->count = 1;
      mat->reserved = 0;
      ++numMats;
      NumOfElements = numMats;
    }
  }

  if ( entity == g_entities )
    printf("%5i unique shaders\n", numMats);

  groups = BuildLightmapGroups(entity);

  if ( !lmFreeBlocks )
    numLightmaps = 0;

  ValidateLightmapGroups(groups);

  /* sort each material's surface list and compute max lightmap area */
  for ( i = 0; i < (int)NumOfElements; i++ )
  {
    mat = &materialGroupSortTable[i];
    mat->head = MergeSortLightmapSurfaces(mat->head, mat->count);
    mat->maxArea = 0;
    for ( surf = mat->head; surf; surf = surf->matListNext )
    {
      area = surf->lightmapWidth * surf->lightmapHeight;
      if ( mat->maxArea < area )
        mat->maxArea = area;
    }
  }

  qsort(materialGroupSortTable, NumOfElements, sizeof(MatSortEntry_t), CompareMaterialGroupsByArea);

  /* allocate lightmaps for each material group */
  totalLmaps = 0;
  for ( i = 0; i < (int)NumOfElements; i++ )
  {
    mat = &materialGroupSortTable[i];
    si = mat->head->shaderInfo;

    Assert(!lmAllowedList, s_assertDisable_AllocateLightmaps);

    allowed = lmAllowedList;
    for ( surf = mat->head; surf; surf = surf->matListNext )
    {
      if ( si->gameFlags & 2 )
      {
        /* surface has lightmap — allocate if not yet done */
        if ( surf->lightStyle == LIGHTSTYLE_NONE )
        {
          AllocateAndBakeGroupLightmap(surf->ownerGroup);
          allowed = lmAllowedList;
        }

        /* track which lightmap indices are used */
        for ( node = allowed; node; node = node->next )
        {
          if ( node->lmapIdx == surf->lightStyle )
            break;
        }
        if ( !node )
        {
          node = malloc(sizeof(*node));
          node->lmapIdx = surf->lightStyle;
          node->next = lmAllowedList;
          lmAllowedList = node;
          allowed = node;
        }
      }
      else
      {
        surf->lightStyle = LIGHTSTYLE_NONE;
        allowed = lmAllowedList;
      }
    }

    /* free allowed list and count unique lightmaps */
    lmapCount = 0;
    while ( allowed )
    {
      node = allowed->next;
      free(allowed);
      allowed = node;
      lmAllowedList = node;
      ++lmapCount;
    }
    totalLmaps += lmapCount;
  }

  if ( entity == g_entities )
    printf("%5i actual shaders\n", totalLmaps);

  /* compute wasted texels in last lightmap */
  wastedTexels = 0;
  for ( block = lmFreeBlocks; block; block = block->next )
  {
    if ( block->lightmapIdx == numLightmaps - 1 )
      wastedTexels += block->height * block->width;
  }

  /* compute and report lightmap efficiency */
  if ( numLightmaps > 1 )
  {
    efficiency = (double)(g_numMapBrushSides + wastedTexels - LIGHTMAP_TEXELS) * (1.0 / LIGHTMAP_TEXELS) / (double)(numLightmaps - 1);
    lightmapEfficiency = efficiency;
  }
  else if ( numLightmaps == 1 )
  {
    efficiency = (double)g_numMapBrushSides * (1.0 / LIGHTMAP_TEXELS);
    lightmapEfficiency = efficiency;
  }

  Com_DPrintf("%7i exact lightmap texels\n", g_numMapBrushSides);
  Com_DPrintf("%7i block lightmap texels\n", numLightmaps * LIGHTMAP_TEXELS);
  return Com_DPrintf("%.2f%% lightmap efficiency\n", lightmapEfficiency);
}

/*
lightmaps.c — Lightmap allocation, UV computation, baking

Reconstructed from cod2map.exe by Rose.
*/

#include "cod4map.h"

LmAllowedNode_t *lmAllowedList;
LmFreeBlock_t   *lmFreeBlocks;

char   g_fmtTwoFloats[8] = { '%', 'g', ',', ' ', '%', 'g', '\0', '\0' };
int    leakFileWritten;

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

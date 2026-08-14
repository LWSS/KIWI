/*
tris_drawsurf.cpp -- native CoD4 transient draw-surface handling.
*/

#include "cod4map.h"

/* CoD4 0x439F20.  The original permanent pool is a 0x44-byte record array.
   KIWI keeps the donor-era DrawSurf_t in a parallel array, so allocation has
   one shared index and the native defaults remain at their original offsets. */
MapDrawSurf_t *AllocMapDrawSurf(void)
{
  MapDrawSurf_t *drawSurf;

  if ( numMapDrawSurfs >= MAX_MAP_DRAW_SURFS )
    Com_Error("MAX_MAP_DRAW_SURFS");

  drawSurf = &g_nativeMapDrawSurfs[numMapDrawSurfs++];
  drawSurf->reflectionProbeIndex = 255;
  drawSurf->lightmapIndex = 256;
  return drawSurf;
}

/* CoD4 0x439C20.  This deliberately retains the final asymmetric entity
   comparison: native qsort receives one entity range, so unequal entity
   numbers are an invariant violation rather than a meaningful sort key. */
int CompareTransientDrawSurfaces(const void *va, const void *vb)
{
  const MapDrawSurf_t *a = (const MapDrawSurf_t *)va;
  const MapDrawSurf_t *b = (const MapDrawSurf_t *)vb;
  float mins[2][3];
  float maxs[2][3];
  int i;
  int j;
  int aIsType16;
  int bIsType16;
  int aNoSurfaceFlag40000;
  int bNoSurfaceFlag40000;

  aIsType16 = (a->material->toolFlagsWord & 0x70) == 0x10;
  bIsType16 = (b->material->toolFlagsWord & 0x70) == 0x10;
  if ( aIsType16 != bIsType16 )
    return aIsType16 ? -1 : 1;

  aNoSurfaceFlag40000 = (a->material->surfaceFlags & 0x40000) == 0;
  bNoSurfaceFlag40000 = (b->material->surfaceFlags & 0x40000) == 0;
  if ( aNoSurfaceFlag40000 != bNoSurfaceFlag40000 )
    return aNoSurfaceFlag40000 ? -1 : 1;

  if ( ((a->material->surfaceFlags & 4) != 0) !=
       ((b->material->surfaceFlags & 4) != 0) )
    return (a->material->surfaceFlags & 4) ? 1 : -1;

  if ( a->isPatch != b->isPatch )
    return a->isPatch ? 1 : -1;
  if ( a->isTerrain != b->isTerrain )
    return a->isTerrain ? 1 : -1;
  if ( a->vertCount != b->vertCount )
    return a->vertCount - b->vertCount;

  for ( i = 0; i < 2; ++i )
  {
    const MapDrawSurf_t *drawSurf = i ? b : a;

    ClearBounds(mins[i], maxs[i]);
    for ( j = 0; j < drawSurf->vertCount; ++j )
      AddPointToBounds(drawSurf->verts[j].pos, mins[i], maxs[i]);
  }

  for ( i = 0; i < 3; ++i )
  {
    if ( mins[1][i] > mins[0][i] )
      return -1;
    if ( mins[1][i] < mins[0][i] )
      return 1;
    if ( maxs[1][i] > maxs[0][i] )
      return -1;
    if ( maxs[1][i] < maxs[0][i] )
      return 1;
  }

  if ( a->material != b->material )
    return IsLoadedMaterialBefore(a->material, b->material) ? -1 : 1;
  if ( a->entityNum == b->entityNum )
    return a->sourceIndex.brushNum - b->sourceIndex.brushNum;
  return 1;
}

/* CoD4 0x439BC0.  qsort operates on native 0x44-byte records in the
   executable.  Sort that exact carrier width, then recover the parallel
   donor-expanded record by matching each unique native record to its saved
   pre-sort index. */
void SortEntityTransientDrawSurfaces(void)
{
  Entity_t *entity = &g_entities[g_currentEntityIndex];
  int first = entity->firstDrawSurf;
  int count = numMapDrawSurfs - first;
  DrawSurf_t *savedExpanded;
  int *savedOutputNum;
  int i;

  if ( !count )
    return;

  savedExpanded = (DrawSurf_t *)malloc(sizeof(*savedExpanded) * count);
  savedOutputNum = (int *)malloc(sizeof(*savedOutputNum) * count);
  if ( !savedExpanded || !savedOutputNum )
    Com_Error("SortEntityTransientDrawSurfaces: out of memory");

  memcpy(savedExpanded, &g_drawSurfs[first],
         sizeof(*savedExpanded) * count);
  for ( i = 0; i < count; ++i )
  {
    savedOutputNum[i] = g_nativeMapDrawSurfs[first + i].outputNum;
    g_nativeMapDrawSurfs[first + i].outputNum = i;
  }

  qsort(&g_nativeMapDrawSurfs[first], count, sizeof(MapDrawSurf_t),
        CompareTransientDrawSurfaces);

  for ( i = 0; i < count; ++i )
  {
    int sourceIndex = g_nativeMapDrawSurfs[first + i].outputNum;

    Assert((unsigned int)sourceIndex < (unsigned int)count, g_assertFlags4);
    g_nativeMapDrawSurfs[first + i].outputNum = savedOutputNum[sourceIndex];
    g_drawSurfs[first + i] = savedExpanded[sourceIndex];
  }
  free(savedOutputNum);
  free(savedExpanded);
}

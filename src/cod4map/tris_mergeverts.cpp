/*
tris_mergeverts.c — Vertex merging and index mapping

Reconstructed from cod2map.exe by Rose.

Finds duplicate vertices within epsilon distance, builds a merge map
to deduplicate them, detects and removes degenerate loops in index
lists, and snaps winding vertices to the shared vertex pool.
*/

#include "cod4map.h"

char s_assertDisable_ApplyMergeMap;
char s_assertDisable_ApplyMergeMap;
char s_assertDisable_BuildMergeMap;
char s_assertDisable_BuildMergeMap;
char s_assertDisable_BuildMergeMap;
char s_assertDisable_BuildMergeMap;
char s_assertDisable_BuildMergeMap;
char s_assertDisable_FindLoopLength;
char s_assertDisable_FindLoops;
char s_assertDisable_FindLoops;
char s_assertDisable_FindLoops;
char s_assertDisable_FindLoops;
char s_assertDisable_FindLoops;
char s_assertDisable_FindLoops;


/*
================
FindLoopLength

Finds length of repeating vertex loop starting at index
================
*/
int FindLoopLength(int start, int *indices, int ptCount)
{
  int length;

  Assert(indices[start] != indices[((start + 1) % ptCount)], s_assertDisable_FindLoopLength);
  length = 2;
  if ( ptCount / 2 < 2 )
    return 1;
  while ( indices[start] != indices[((length + start) % ptCount)] )
  {
    if ( ++length > ptCount / 2 )
      return 1;
  }
  return length;
}

/*
================
FindLoops

Scans winding for all repeating vertex loops
================
*/
int FindLoops( int *loopLength, int ptCount, int *indices, int *loopStart, int loopLimit )
{
  int loopCount = 0;
  int i, len;

  Assert( ptCount >= 3, s_assertDisable_FindLoops );
  Assert( indices, s_assertDisable_FindLoops );
  Assert( loopStart, s_assertDisable_FindLoops );
  Assert( loopLength, s_assertDisable_FindLoops );
  Assert( loopLimit > 0, s_assertDisable_FindLoops );

  for ( i = 0; i < ptCount; i++ )
  {
    len = FindLoopLength( i, indices, ptCount );
    Assert( len >= 1 && len < ptCount || s_assertDisable_FindLoops, s_assertDisable_FindLoops );

    if ( len > 1 )
    {
      if ( loopCount == loopLimit )
        Com_Error( "More than %i loops in a winding", loopLimit );
      loopStart[loopCount] = i;
      loopLength[loopCount] = len;
      loopCount++;
    }
  }

  return loopCount;
}

/*
================
LoopMatches

Checks if two loops at different starts have identical content
================
*/
char LoopMatches(int startA, int startB, int *indices, int ptCount, int length)
{
  int k, ofs = startB - startA;

  for ( k = 0; k < length; k++ )
    if ( indices[(startA + k) % ptCount] != indices[(startA + k + ofs) % ptCount] )
      return 0;
  return 1;
}

/*
================
BuildMergeMap

Builds spatial hash merge map: maps each vertex to canonical representative
================
*/
int *BuildMergeMap( char *baseVert, int coordCount, unsigned int vertStride, int vertCount, float epsilon )
{
  int *indexMap, *hashNext;
  float *vert;
  int i, j, bucket;
  int xMin, xMax, yMin, yMax, zMin, zMax;
  int ix, iy, iz, zHash;
  int hash, yHash;

  Assert( epsilon < 0.5f, s_assertDisable_BuildMergeMap );
  Assert( baseVert != NULL, s_assertDisable_BuildMergeMap );
  Assert( coordCount == 2 || coordCount == 3, s_assertDisable_BuildMergeMap );
  Assert( vertStride >= (unsigned int)(4 * coordCount), s_assertDisable_BuildMergeMap );
  Assert( vertCount >= 0, s_assertDisable_BuildMergeMap );

  memset( g_vertexHashTable, 0xFF, sizeof(g_vertexHashTable) );
  indexMap = malloc( sizeof(int) * vertCount );
  hashNext = malloc( sizeof(int) * vertCount );

  for ( i = 0; i < vertCount; i++ )
  {
    vert = (float *)&baseVert[i * vertStride];

    /* Native 0x459D50: -25037*x + 25031*y + 15473*z, reduced to 14 bits. */
    hash = VERT_HASH_PRIME_Y * RoundFloatToInt( vert[1] ) - VERT_HASH_PRIME_X * RoundFloatToInt( vert[0] );
    if ( coordCount == 3 )
      bucket = (hash + VERT_HASH_PRIME_Z * RoundFloatToInt( vert[2] )) & (VERTEX_HASH_TABLE_SIZE - 1);
    else
      bucket = hash & (VERTEX_HASH_TABLE_SIZE - 1);

    hashNext[i] = g_vertexHashTable[bucket];
    g_vertexHashTable[bucket] = i;
    indexMap[i] = i;

    /* integer coordinate ranges for epsilon neighborhood */
    xMin = RoundFloatToInt( vert[0] - epsilon );
    xMax = RoundFloatToInt( vert[0] + epsilon );
    yMin = RoundFloatToInt( vert[1] - epsilon );
    yMax = RoundFloatToInt( vert[1] + epsilon );
    if ( coordCount == 3 )
    {
      zMin = RoundFloatToInt( vert[2] - epsilon );
      zMax = RoundFloatToInt( vert[2] + epsilon );
    }
    else
    {
      zMin = zMax = 0;
    }

    /* search all hash buckets in the neighborhood for matching verts */
    for ( iz = zMin; iz <= zMax; iz++ )
    {
      zHash = VERT_HASH_PRIME_Z * iz;
      for ( iy = yMin; iy <= yMax; iy++ )
      {
        yHash = VERT_HASH_PRIME_Y * iy;
        for ( ix = xMin; ix <= xMax; ix++ )
        {
          hash = yHash - VERT_HASH_PRIME_X * ix + zHash;
          for ( j = g_vertexHashTable[hash & (VERTEX_HASH_TABLE_SIZE - 1)]; j >= 0; j = hashNext[j] )
          {
            if ( j == i )
              continue;
            if ( VectorCompareEpsilon( (float *)&baseVert[vertStride * j], vert, epsilon, coordCount ) )
              indexMap[i] = indexMap[j];
          }
        }
      }
    }
  }

  free( hashNext );
  return indexMap;
}

/*
================
ApplyMergeMap

Averages merged vertex positions and snaps to 1/1024 grid
================
*/
void ApplyMergeMap(char *baseVert, int vertStride, int vertCount, int startIndex, int *indexMap)
{
  float (*posAccum)[3];
  int *addendCount;
  float *srcVert;
  float *curVert;
  int i, mapIdx;
  double invCount;
  float snapVal;

  posAccum = malloc(sizeof(vec3_t) * vertCount);
  memset(posAccum, 0, sizeof(vec3_t) * vertCount);
  addendCount = malloc(sizeof(int) * vertCount);
  memset(addendCount, 0, sizeof(int) * vertCount);

  /* accumulate vertex positions into merge targets */
  srcVert = (float *)&baseVert[startIndex * vertStride];
  for ( i = startIndex; i < vertCount; i++ )
  {
    mapIdx = indexMap[i];
    posAccum[mapIdx][0] = (float)((double)posAccum[mapIdx][0] + srcVert[0]);
    posAccum[mapIdx][1] = (float)((double)posAccum[mapIdx][1] + srcVert[1]);
    posAccum[mapIdx][2] = (float)((double)posAccum[mapIdx][2] + srcVert[2]);
    addendCount[mapIdx]++;
    srcVert = (float *)((char *)srcVert + vertStride);
  }

  /* average accumulated positions and snap to MERGE_SNAP_SCALE grid */
  curVert = (float *)&baseVert[startIndex * vertStride];
  for ( i = startIndex; i < vertCount; i++ )
  {
    Assert(indexMap[i] <= i, s_assertDisable_ApplyMergeMap);

    if ( indexMap[i] == i )
    {
      Assert(addendCount[i], s_assertDisable_ApplyMergeMap);
      invCount = 1.0 / (double)addendCount[i];
      posAccum[i][0] = (float)((double)posAccum[i][0] * invCount);
      posAccum[i][1] = (float)((double)posAccum[i][1] * invCount);
      posAccum[i][2] = (float)((double)posAccum[i][2] * invCount);

      /* snap each component to 1/1024 grid */
      #define MERGE_SNAP_SCALE     1024.0
      #define MERGE_SNAP_INV       (1.0 / MERGE_SNAP_SCALE)
      snapVal = posAccum[i][0] * MERGE_SNAP_SCALE;
      posAccum[i][0] = (double)RoundFloatToInt(snapVal) * MERGE_SNAP_INV;
      snapVal = posAccum[i][1] * MERGE_SNAP_SCALE;
      posAccum[i][1] = (double)RoundFloatToInt(snapVal) * MERGE_SNAP_INV;
      snapVal = posAccum[i][2] * MERGE_SNAP_SCALE;
      posAccum[i][2] = (double)RoundFloatToInt(snapVal) * MERGE_SNAP_INV;
    }

    /* write merged position back to vertex buffer */
    curVert[0] = posAccum[indexMap[i]][0];
    curVert[1] = posAccum[indexMap[i]][1];
    curVert[2] = posAccum[indexMap[i]][2];
    curVert = (float *)((char *)curVert + vertStride);
  }

  free(addendCount);
  free(posAccum);
}

/*
================
RemoveLoops

Removes one pair of matching duplicate loops from index array
================
*/
int RemoveLoops( int ptCount, int *indices, intptr_t auxData, int auxElemSize, int *loopStarts, int *loopLengths, int loopCount )
{
  int i, j, start, len, remain;

  /* for each loop, search for a duplicate with matching length */
  for ( i = 0; i < loopCount; i++ )
  {
    start = loopStarts[i];
    len = loopLengths[i];

    for ( j = i + 1; j < loopCount; j++ )
    {
      if ( len != loopLengths[j] )
        continue;
      if ( !LoopMatches( loopStarts[j], start, indices, ptCount, len ) )
        continue;

      /* found duplicate — remove by shifting data left */
      if ( len + start > ptCount )
        start = loopStarts[j];
      remain = ptCount - len - start;
      memmove( &indices[start], &indices[start + len], sizeof(int) * remain );
      if ( auxElemSize )
        memmove( (void *)(auxData + auxElemSize * start), (const void *)(auxData + auxElemSize * (start + len)), auxElemSize * remain );
      return ptCount - len;
    }
  }

  return ptCount;
}

/*
================
RemoveDuplicateLoops

Iteratively removes all duplicate vertex loops
================
*/
int RemoveDuplicateLoops( int ptCount, int auxElemSize, int *indices, intptr_t auxData )
{
  int loopLengths[MAX_LOOPS];
  int loopStarts[MAX_LOOPS];
  int numLoops, newCount;

  if ( ptCount < 6 )
    return ptCount;

  /* iteratively find and remove duplicate loops until none remain */
  numLoops = FindLoops( loopLengths, ptCount, indices, loopStarts, MAX_LOOPS );
  while ( numLoops > 1 )
  {
    newCount = RemoveLoops( ptCount, indices, auxData, auxElemSize, loopStarts, loopLengths, numLoops );
    if ( newCount == ptCount )
      break;
    ptCount = newCount;
    numLoops = FindLoops( loopLengths, ptCount, indices, loopStarts, MAX_LOOPS );
  }

  return ptCount;
}

/*
================
SnapAndMergeWinding

Maps winding through merge map, deduplicates and strips loops
================
*/
int SnapAndMergeWinding( int firstVert, int ptCount, int *indexMap, int *outIndices, char *auxData, int auxElemSize )
{
  int *out = outIndices;
  int src, dst, head;

  /* trim tail vertices that match the head after merge mapping */
  head = indexMap[firstVert];
  while ( ptCount > 0 && indexMap[firstVert + ptCount - 1] == head )
    ptCount--;

  /* compact: copy mapped indices, skip consecutive duplicates */
  out[0] = head;
  dst = 0;
  for ( src = 1; src < ptCount; src++ )
  {
    if ( indexMap[firstVert + src] == out[dst] )
      continue;
    dst++;
    out[dst] = indexMap[firstVert + src];
    if ( auxElemSize && src != dst )
      AuxDataCopy( auxElemSize, (char *)auxData, src, 1, (char *)auxData, dst );
  }

  /* remove remaining duplicates and loops */
  dst = RemoveDuplicateIndices( dst + 1, out, auxData, auxElemSize );
  return RemoveDuplicateLoops( dst, auxElemSize, out, (intptr_t)auxData );
}

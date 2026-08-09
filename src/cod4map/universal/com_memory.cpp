/*
com_memory.c — Memory management

Reconstructed from cod2map.exe by Rose.
*/

#include "cod4map.h"

intptr_t com_fileDataHashTable[FILE_DATA_HASH_SIZE];
intptr_t com_hunkData;
char     g_heapPool0[HEAP_POOL_SMALL];
char     g_heapPool1[HEAP_POOL_SMALL];
char     g_heapPool2[HEAP_POOL_LARGE];
char     g_heapPool3[HEAP_POOL_LARGE];
char     g_heapPool4[HEAP_POOL_LARGE];
char     g_heapPool5[HEAP_POOL_LARGE];
char     g_heapPool6[HEAP_POOL_LARGE];
int      hunk_high_permanent;
int      hunk_high_temp;
int      hunk_low_permanent;
int      hunk_low_temp;
intptr_t s_hunkData;
int      s_hunkTotal;

char s_assertDisable_Hunk_AllocAlign;
char s_assertDisable_Hunk_AllocAlign;
char s_assertDisable_Hunk_AllocAlign;
char s_assertDisable_Hunk_AllocAlign;
char s_assertDisable_Hunk_AllocAlign;
char s_assertDisable_Hunk_AllocLowAlign;
char s_assertDisable_Hunk_AllocLowAlign;
char s_assertDisable_Hunk_AllocLowAlign;
char s_assertDisable_Hunk_AllocLowAlign;
char s_assertDisable_Hunk_AllocLowAlign;
char s_assertDisable_Hunk_AllocateTempMemory;
char s_assertDisable_Hunk_AllocateTempMemory;
char s_assertDisable_Hunk_CheckTempMemoryHigh;
char s_assertDisable_Hunk_CheckTempMemoryHigh;
char s_assertDisable_Hunk_CheckTempMemoryHigh;
char s_assertDisable_Hunk_CheckTempMemoryLow;
char s_assertDisable_Hunk_CheckTempMemoryLow;
char s_assertDisable_Hunk_CheckTempMemoryLow;
char s_assertDisable_Hunk_Clear;
char s_assertDisable_Hunk_ClearTempMemory;
char s_assertDisable_Hunk_FreeTempMemory;
char s_assertDisable_Hunk_FreeTempMemory;
char s_assertDisable_Hunk_PurgeFreeListRange;
char s_assertDisable_CopyStringHunk;
char s_assertDisable_Z_Free;


/*
================
Z_Free

Frees a memory block allocated by Z_Malloc
================
*/
void Z_Free(void *Block)
{
  /* Native 0x412D80 rejects NULL before forwarding to its raw free bridge. */
  Assert(Block, s_assertDisable_Z_Free);
  free(Block);
}

/*
================
Z_MallocFailed

Reports Z_Malloc allocation failure — asserts and calls Com_Error
================
*/
void Z_MallocFailed(int size)
{
  if ( !g_assertsDisabled )
  {
    int assertResult = AssertFailed(
      va("Z_Malloc: failed to allocate %d bytes", size),
      SRCFILE,
      __LINE__,
      0,
      1);
    if ( !assertResult )
      DebugBreak();
  }
  Sys_Printf("Z_Malloc: failed to allocate %d bytes\n", size);
  Com_ErrorLevel(0, "Z_Malloc: EXE_ERR_OUT_OF_MEMORY");
}

/*
================
Z_Malloc

Allocates zero-initialized memory — malloc + Com_Memset(0), calls Z_MallocFailed on failure
================
*/
void *Z_Malloc(size_t size)
{
  void *buf;

  buf = malloc(size);
  if ( buf )
  {
    Com_Memset(buf, 0, (int)size);
    return buf;
  }
  Z_MallocFailed((int)size);
  return NULL;
}

/*
================
Hunk_FindDataForFileInternal_Lookup

Looks up cached file data by hash index, type, and name
================
*/
/* hunkFileEntry_t */
typedef struct HunkFileEntry_s {
    int                      data;    /* [0] cached data pointer */
    struct HunkFileEntry_s  *next;    /* [4] next in hash chain */
    unsigned char            type;    /* [8] file type */
    char                     name[1]; /* [9] file name (variable length) */
} hunkFileEntry_t;

int Hunk_FindDataForFileInternal_Lookup(int hashIndex, int type, const char *name)
{
  hunkFileEntry_t *entry;

  for ( entry = (hunkFileEntry_t *)com_fileDataHashTable[hashIndex]; entry; entry = entry->next )
  {
    if ( entry->type == type && !_stricmp(entry->name, name) )
      return entry->data;
  }
  return 0;
}

/*
================
Hunk_PurgeFreeListRange

Removes free list entries in [lowAddr, highAddr) range
================
*/
void Hunk_PurgeFreeListRange(hunkFileEntry_t **listHead, unsigned int highAddr, unsigned int lowAddr)
{
  hunkFileEntry_t *entry;

  Assert(Sys_IsMainThread(), s_assertDisable_Hunk_PurgeFreeListRange);
  while ( *listHead )
  {
    entry = *listHead;
    if ( (uintptr_t)entry < lowAddr || (uintptr_t)entry >= highAddr )
      listHead = &entry->next;
    else
      *listHead = entry->next;
  }
}

/*
================
Hunk_ClearTempMemory

Purges all temp memory entries from the free list hash table
================
*/
void Hunk_ClearTempMemory()
{
  unsigned int low, high;
  int i;

  Assert(Sys_IsMainThread(), s_assertDisable_Hunk_ClearTempMemory);

  low = (unsigned int)(hunk_low_permanent + s_hunkData);
  high = (unsigned int)(s_hunkData + s_hunkTotal - hunk_high_permanent);

  /* purge all hash buckets */
  #define HUNK_HASH_SIZE 1024
  for ( i = 0; i < HUNK_HASH_SIZE; i++ )
    Hunk_PurgeFreeListRange((hunkFileEntry_t **)&com_fileDataHashTable[i], high, low);

  Hunk_PurgeFreeListRange((hunkFileEntry_t **)&com_hunkData, high, low);
}

/*
================
Hunk_Clear

Resets all hunk allocation counters (low/high permanent and temp) to zero
================
*/
void Hunk_Clear()
{
  Assert(Sys_IsMainThread(), s_assertDisable_Hunk_Clear);
  hunk_low_permanent = 0;
  hunk_low_temp = 0;
  hunk_high_permanent = 0;
  hunk_high_temp = 0;
  Hunk_ClearTempMemory();
}

/*
================
Hunk_AllocateTempMemory

Allocates temp memory from hunk low end (16-byte header + aligned data), falls back to malloc
================
*/
/* hunkHeader_t */
typedef struct hunkHeader_s {
    int sentinel;    /* [0]  magic cookie (ALLOC or FREE) */
    int size;        /* [4]  total allocation size including header */
    int reserved[2]; /* [8]  padding to 16-byte alignment */
} hunkHeader_t;
int *Hunk_AllocateTempMemory(size_t size)
{
  unsigned int aligned;
  hunkHeader_t *header;
  int newLowTemp, prevLowTemp;
  void *userData;

  Assert(Sys_IsMainThread(), s_assertDisable_Hunk_AllocateTempMemory);

  /* fallback to malloc if hunk not initialized */
  if ( !s_hunkData )
  {
    void *buf = malloc(size);
    if ( buf )
    {
      Com_Memset(buf, 0, (int)size);
      return buf;
    }
    Z_MallocFailed((int)size);
    return NULL;
  }

  /* align to 16-byte boundary, allocate header + data */
  #define HUNK_ALIGNMENT       16
  #define HUNK_ALIGNMENT_MASK  (HUNK_ALIGNMENT - 1)
  #define HUNK_HEADER_SIZE     ((int)sizeof(hunkHeader_t))
  #define HUNK_SENTINEL_ALLOC  0x89ABCDF2
  #define HUNK_SENTINEL_FREE   0x89ABCDF3

  aligned = (hunk_low_temp + HUNK_ALIGNMENT_MASK) & ~HUNK_ALIGNMENT_MASK;
  header = (hunkHeader_t *)(aligned + s_hunkData);
  newLowTemp = (int)(size + HUNK_HEADER_SIZE + aligned);
  prevLowTemp = hunk_low_temp;
  hunk_low_temp = newLowTemp;

  if ( hunk_high_temp + newLowTemp > s_hunkTotal )
    Com_ErrorLevel(1,
      "Hunk_AllocateTempMemory: failed on %i bytes (total %i MB, low %i MB, high %i MB), needs %i more hunk bytes",
      size + HUNK_HEADER_SIZE, s_hunkTotal >> 20, newLowTemp >> 20, hunk_high_temp >> 20,
      newLowTemp + hunk_high_temp - s_hunkTotal);

  /* user data starts after 16-byte header (4 ints) */
  userData = header + 1;

  /* assert alignment */
  Assert(!(((uintptr_t)userData & HUNK_ALIGNMENT_MASK)), s_assertDisable_Hunk_AllocateTempMemory);

  /* write sentinel and allocation size into header */
  header->sentinel = HUNK_SENTINEL_ALLOC;
  header->size = hunk_low_temp - prevLowTemp;
  return userData;
}

/*
================
Hunk_FreeTempMemory

Frees temp memory — validates magic cookie, adjusts hunk_low.temp
================
*/
void Hunk_FreeTempMemory(void *buf)
{
  hunkHeader_t *header;

  Assert(Sys_IsMainThread(), s_assertDisable_Hunk_FreeTempMemory);

  if ( !s_hunkData )
  {
    free(buf);
    return;
  }

  /* header is directly before user data */
  header = (hunkHeader_t *)buf - 1;
  if ( header->sentinel != HUNK_SENTINEL_ALLOC )
    Com_ErrorLevel(0, "Hunk_FreeTempMemory: bad magic");

  /* mark as freed */
  header->sentinel = HUNK_SENTINEL_FREE;

  /* assert header is at expected position */
  Assert(header == (hunkHeader_t *)(s_hunkData + ((hunk_low_temp - header->size + HUNK_ALIGNMENT_MASK) & ~HUNK_ALIGNMENT_MASK)), s_assertDisable_Hunk_FreeTempMemory);
  hunk_low_temp -= header->size;
}

/*
================
Hunk_CheckTempMemoryLow

Asserts low hunk temp equals permanent (no outstanding temp allocations)
================
*/
void Hunk_CheckTempMemoryLow()
{

  Assert(Sys_IsMainThread(), s_assertDisable_Hunk_CheckTempMemoryLow);
  Assert(s_hunkData, s_assertDisable_Hunk_CheckTempMemoryLow);

  Assert(!(hunk_low_temp != hunk_low_permanent), s_assertDisable_Hunk_CheckTempMemoryLow);
}

/*
================
Hunk_CheckTempMemoryHigh

Asserts high hunk temp equals permanent (no outstanding temp allocations)
================
*/
void Hunk_CheckTempMemoryHigh()
{

  Assert(Sys_IsMainThread(), s_assertDisable_Hunk_CheckTempMemoryHigh);
  Assert(s_hunkData, s_assertDisable_Hunk_CheckTempMemoryHigh);

  Assert(!(hunk_high_temp != hunk_high_permanent), s_assertDisable_Hunk_CheckTempMemoryHigh);
}

/*
================
CopyStringHunk

Allocates and copies a string (strdup variant with Com_Memset)
================
*/
char *CopyStringHunk(const char *str)
{
  int len;
  char *copy;

  Assert(str, s_assertDisable_CopyStringHunk);
  len = (int)strlen(str) + 1;
  copy = (char *)Z_Malloc(len);
  strcpy(copy, str);
  return copy;
}

/*
================
Hunk_AllocAlign

Allocates from high hunk end with specified alignment, growing downward
================
*/
intptr_t Hunk_AllocAlign(unsigned int size, int alignment)
{
  int alignMask;
  int newHighPerm;
  intptr_t allocPtr;

  Assert(Sys_IsMainThread(), s_assertDisable_Hunk_AllocAlign);
  Assert(s_hunkData, s_assertDisable_Hunk_AllocAlign);

  alignMask = alignment - 1;
  Assert(((alignMask) & alignment) == 0, s_assertDisable_Hunk_AllocAlign);
  #define HUNK_MAX_ALIGNMENT 4096
  Assert(!(alignment > HUNK_MAX_ALIGNMENT), s_assertDisable_Hunk_AllocAlign);

  Hunk_CheckTempMemoryHigh();

  /* allocate from high end, growing downward */
  newHighPerm = ~alignMask & (alignMask + size + hunk_high_permanent);
  allocPtr = s_hunkData + s_hunkTotal - newHighPerm;
  hunk_high_permanent = newHighPerm;
  hunk_high_temp = newHighPerm;

  if ( hunk_low_temp + newHighPerm > s_hunkTotal )
    Com_ErrorLevel(1, "Hunk_AllocAlign failed on %i bytes (total %i MB, low %i MB, high %i MB)",
      size, s_hunkTotal >> 20, hunk_low_temp >> 20, newHighPerm >> 20);

  /* assert alignment */
  Assert(!((allocPtr & alignMask)), s_assertDisable_Hunk_AllocAlign);

  memset((void *)allocPtr, 0, size);
  return allocPtr;
}

/*
================
Hunk_AllocLowAlign

Allocates from low hunk end with specified alignment, growing upward.
================
*/
intptr_t Hunk_AllocLowAlign(unsigned int size, int alignment)
{
  int alignMask;
  int aligned, newLowPerm;
  intptr_t allocPtr;

  Assert(Sys_IsMainThread(), s_assertDisable_Hunk_AllocLowAlign);
  Assert(s_hunkData, s_assertDisable_Hunk_AllocLowAlign);

  alignMask = alignment - 1;

  /* assert power of two */
  Assert(!(alignMask & alignment), s_assertDisable_Hunk_AllocLowAlign);
  Assert(!(alignment > HUNK_MAX_ALIGNMENT), s_assertDisable_Hunk_AllocLowAlign);

  Hunk_CheckTempMemoryLow();

  /* allocate from low end, growing upward */
  aligned = ~alignMask & (hunk_low_permanent + alignMask);
  allocPtr = aligned + s_hunkData;
  newLowPerm = size + aligned;
  hunk_low_permanent = newLowPerm;
  hunk_low_temp = newLowPerm;

  if ( hunk_high_temp + newLowPerm > s_hunkTotal )
    Com_ErrorLevel(1, "Hunk_AllocLowAlign failed on %i bytes (total %i MB, low %i MB, high %i MB)",
      size, s_hunkTotal >> 20, newLowPerm >> 20, hunk_high_temp >> 20);

  /* assert alignment */
  Assert(!((allocPtr & alignMask)), s_assertDisable_Hunk_AllocLowAlign);

  memset((void *)allocPtr, 0, size);
  return allocPtr;
}

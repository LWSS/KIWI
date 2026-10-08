/*
com_memory.c — Memory management

Reconstructed from cod2map.exe by Rose.
*/

#include "cod4map.h"

int      hunk_high_temp;
int      hunk_low_temp;
intptr_t s_hunkData;
int      s_hunkTotal;


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
void Z_MallocFailed(size_t size)
{
  vassert( alwaysfails, "Z_Malloc: failed to allocate %zu bytes", size );
  Sys_Printf("Z_Malloc: failed to allocate %zu bytes\n", size);
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
    memset(buf, 0, size);
    return buf;
  }
  Z_MallocFailed(size);
  return NULL;
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
      memset(buf, 0, size);
      return buf;
    }
    Z_MallocFailed(size);
    return NULL;
  }

  /* align to 16-byte boundary, allocate header + data */
  #define HUNK_ALIGNMENT       16
  #define HUNK_ALIGNMENT_MASK  (HUNK_ALIGNMENT - 1)
  #define HUNK_HEADER_SIZE     ((int)sizeof(hunkHeader_t))
  #define HUNK_SENTINEL_ALLOC  0x89ABCDF2
  #define HUNK_SENTINEL_FREE   0x89ABCDF3

  if (size > INT_MAX - HUNK_HEADER_SIZE - HUNK_ALIGNMENT_MASK
      || hunk_low_temp > INT_MAX - HUNK_ALIGNMENT_MASK)
    Com_ErrorLevel(1, "Hunk_AllocateTempMemory: allocation too large (%zu bytes)", size);
  aligned = (hunk_low_temp + HUNK_ALIGNMENT_MASK) & ~HUNK_ALIGNMENT_MASK;
  if (aligned > (unsigned int)(s_hunkTotal - hunk_high_temp)
      || size + HUNK_HEADER_SIZE > (size_t)(s_hunkTotal - hunk_high_temp) - aligned)
    Com_ErrorLevel(1, "Hunk_AllocateTempMemory: hunk exhausted (%zu bytes)", size);
  header = (hunkHeader_t *)(aligned + s_hunkData);
  newLowTemp = (int)(size + HUNK_HEADER_SIZE + aligned);
  prevLowTemp = hunk_low_temp;
  hunk_low_temp = newLowTemp;

  if ( hunk_high_temp + newLowTemp > s_hunkTotal )
    Com_ErrorLevel(1,
      "Hunk_AllocateTempMemory: failed on %zu bytes (total %i MB, low %i MB, high %i MB), needs %i more hunk bytes",
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

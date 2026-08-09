/*
com_shared.c — Shared utilities

Reconstructed from cod2map.exe by Rose.
*/

#include "cod4map.h"

char s_assertDisable_Com_Memcpy;


/*
================
Com_Error

Error function. Calls registered error handler if set,
otherwise prints to stderr and exits.
================
*/
int Com_Error(const char *fmt, ...)
{
  va_list args;

  va_start(args, fmt);
  if ( g_errorHandler )
    g_errorHandler(fmt, args);
  va_end(args);
  return 0;
}

/*
================
Com_SetErrorHandler

Sets a custom error callback for Com_Error.
cod2map-specific (not in q3/CoD2 server).
================
*/
void Com_SetErrorHandler(void (*handler)(const char *, va_list))
{
  g_errorHandler = handler;
}

/*
================
Com_DPrintf

Debug printf - only prints if verbose mode is enabled.
================
*/
int Com_DPrintf(const char *fmt, ...)
{
  va_list args;

  va_start(args, fmt);
  if ( verbose )
    return vprintf(fmt, args);
  return 0;
}

/*
================
Com_Printf

Main console output. Always prints to stdout, also sends
to CoD2Map Process Server window via Win32 IPC.
================
*/
int Com_Printf(const char *fmt, ...)
{
  char buf[4100];
  va_list args;

  va_start(args, fmt);
  vsprintf(buf, fmt, args);
  printf(buf);
  return fflush(stdout);
}

/*
================
CopyString

Allocates memory and copies a string (equivalent to strdup).
================
*/
char *CopyString(const char *str)
{
  char *copy;
  int len = (int)strlen(str) + 1;

  copy = malloc(len);
  if ( !copy )
    Z_MallocFailed(len);
  memset(copy, 0, len);
  strcpy(copy, str);
  return copy;
}

/*
================
Sys_Printf

System printf - always prints to stdout.
================
*/
int Sys_Printf(const char *fmt, ...)
{
  va_list args;

  va_start(args, fmt);
  return vprintf(fmt, args);
}

/* CoD4 0x4765E0: touch at most 4096 bytes in 32-byte cache-line steps. */
void Com_Prefetch(const void *src, int count)
{
  const volatile unsigned char *cursor;
  int touchCount;

  cursor = (const volatile unsigned char *)src;
  touchCount = (min(count, 4096) + 31) >> 5;
  while ( touchCount-- > 0 )
  {
    (void)*cursor;
    cursor += 32;
  }
}

/* CoD4 0x4762C0: cache-touch, then copy descending 32-byte blocks and a tail. */
void Com_Memcpy( void *dest, const void *src, int count )
{
  unsigned char *out;
  const unsigned char *in;
  int bulkCount;
  int offset;

  Assert( src || !count, s_assertDisable_Com_Memcpy );
  Assert( dest || !count, s_assertDisable_Com_Memcpy );

  Com_Prefetch(src, count);
  out = (unsigned char *)dest;
  in = (const unsigned char *)src;

  if ( count >= 32 )
  {
    bulkCount = count & ~31;
    for ( offset = bulkCount - 32; offset >= 0; offset -= 32 )
    {
      ((unsigned int *)(out + offset))[0] = ((const unsigned int *)(in + offset))[0];
      ((unsigned int *)(out + offset))[1] = ((const unsigned int *)(in + offset))[1];
      ((unsigned int *)(out + offset))[2] = ((const unsigned int *)(in + offset))[2];
      ((unsigned int *)(out + offset))[3] = ((const unsigned int *)(in + offset))[3];
      ((unsigned int *)(out + offset))[4] = ((const unsigned int *)(in + offset))[4];
      ((unsigned int *)(out + offset))[5] = ((const unsigned int *)(in + offset))[5];
      ((unsigned int *)(out + offset))[6] = ((const unsigned int *)(in + offset))[6];
      ((unsigned int *)(out + offset))[7] = ((const unsigned int *)(in + offset))[7];
    }
    out += bulkCount;
    in += bulkCount;
    count &= 31;
  }

  if ( count >= 16 )
  {
    ((unsigned int *)out)[0] = ((const unsigned int *)in)[0];
    ((unsigned int *)out)[1] = ((const unsigned int *)in)[1];
    ((unsigned int *)out)[2] = ((const unsigned int *)in)[2];
    ((unsigned int *)out)[3] = ((const unsigned int *)in)[3];
    out += 16;
    in += 16;
    count -= 16;
  }
  if ( count >= 8 )
  {
    ((unsigned int *)out)[0] = ((const unsigned int *)in)[0];
    ((unsigned int *)out)[1] = ((const unsigned int *)in)[1];
    out += 8;
    in += 8;
    count -= 8;
  }
  if ( count >= 4 )
  {
    *(unsigned int *)out = *(const unsigned int *)in;
    out += 4;
    in += 4;
    count -= 4;
  }
  if ( count >= 2 )
  {
    *(unsigned short *)out = *(const unsigned short *)in;
    if ( count >= 3 )
      out[2] = in[2];
  }
  else if ( count == 1 )
  {
    *out = *in;
  }
}

/*
================
Com_Memset4

Fills memory with a 4-byte repeated value.
================
*/
int Com_Memset4( void *dest, int fillValue, int count )
{
  int *p = dest;
  int i;

  for ( i = 0; i < count; i++ )
    p[i] = fillValue;

  return fillValue;
}

/*
================
Com_Memset

Fills memory byte-by-byte using 4-byte packed writes.
================
*/
int Com_Memset( void *dest, int fillByte, int byteCount )
{
  unsigned char *p;
  unsigned short packedWord;
  int packedValue, i;

  packedWord = (unsigned short)(fillByte | (fillByte << 8));
  packedValue = packedWord | (packedWord << 16);

  Com_Memset4( dest, packedValue, byteCount / 4 );

  p = (unsigned char *)dest + (byteCount & ~3);
  for ( i = 0; i < (byteCount & 3); i++ )
    p[i] = fillByte;

  return packedValue;
}

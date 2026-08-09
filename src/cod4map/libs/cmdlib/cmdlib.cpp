/*
cmdlib.c — Command line utilities and file I/O

Reconstructed from cod2map.exe by Rose.
*/

#include "cod4map.h"
#include <mmsystem.h>

#pragma comment(lib, "winmm.lib")

char s_assertDisable_FS_GetPreviousPathComponent;


/*
================
SafeMalloc

Allocates page-aligned, zero-initialized memory
================
*/
void *SafeMalloc(int size)
{
  int aligned;
  void *ptr;

  /* round up to page boundary */
  aligned = size;
  if ( size % MEM_PAGE_SIZE )
    aligned += MEM_PAGE_SIZE - (size % MEM_PAGE_SIZE);
  ptr = malloc(aligned);
  memset(ptr, 0, aligned);
  return ptr;
}

/*
================
FS_FileLength

Returns the length of an open file in bytes
================
*/
int FS_FileLength(FILE *f)
{
  int pos;
  int end;

  pos = ftell(f);
  fseek(f, 0, SEEK_END);
  end = ftell(f);
  fseek(f, pos, SEEK_SET);
  return end;
}

/*
================
SafeOpenRead

Opens a file for reading, calls Com_Error on failure
================
*/
FILE *SafeOpenRead(char *filename)
{
  FILE *f;
  int *errnoPtr;
  char *errorStr;

  f = fopen(filename, "rb");
  if ( !f )
  {
    errnoPtr = _errno();
    errorStr = strerror(*errnoPtr);
    Com_Error("Error opening %s: %s", filename, errorStr);
  }
  return f;
}

/* CoD4 0x40DF90. */
FILE *SafeOpenWrite(char *filename)
{
  FILE *f;
  int *errnoPtr;
  char *errorStr;

  f = fopen(filename, "wb");
  if ( !f )
  {
    errnoPtr = _errno();
    errorStr = strerror(*errnoPtr);
    Com_Error("Error opening %s: %s", filename, errorStr);
  }
  return f;
}

/*
================
SafeRead

Reads exactly ElementCount bytes from file, asserts count > 0
================
*/
int SafeRead(FILE *fp, void *buf, int count)
{
  int bytesRead;

  Assert(count > 0, g_fsPathFlags0);
  bytesRead = (int)fread_locked(buf, 1, count, fp);
  if ( bytesRead != count )
    return Com_Error("File read failure - read %i of %i bytes", bytesRead, count);
  return bytesRead;
}

/*
================
SafeWrite

Writes exactly ElementCount bytes to file, asserts count >= 0
================
*/
int SafeWrite(FILE *fp, void *buf, int count)
{
  int bytesWritten;

  Assert(count >= 0, g_fsPathFlags1);
  bytesWritten = (int)fwrite_locked(buf, 1, count, fp);
  if ( bytesWritten != count )
    return Com_Error("File write failure - wrote %i of %i bytes", bytesWritten, count);
  return bytesWritten;
}

/*
================
LoadFile

Loads an entire file into a newly allocated buffer
================
*/
int LoadFile(char *filename, void **bufferptr)
{
  FILE *f;
  int length;
  unsigned char *buf;

  *bufferptr = NULL;
  if ( !filename || !strlen(filename) )
    return -1;

  f = fopen(filename, "rb");
  if ( !f )
    return -1;

  /* read entire file into buffer */
  length = FS_FileLength(f);
  buf = SafeMalloc(length + 1);
  buf[length] = '\0';
  if ( length )
    SafeRead(f, buf, length);
  fclose(f);
  *bufferptr = buf;
  return length;
}

/*
================
DefaultExtension

Appends extension to path if it doesn't already have one
================
*/
void DefaultExtension(char *path, const char *extension)
{
  char *p;

  /* scan backwards for '.' or '/' */
  for ( p = path + strlen(path) - 1; p >= path; p-- )
  {
    if ( *p == '/' )
      break;
    if ( *p == '.' )
      return;  /* already has extension */
  }

  /* append extension */
  strcat(path, extension);
}

/*
================
FS_ExtractBasename

Extracts the filename (without path) from a full path string.
Searches backwards for '/' or '\\' and copies everything after it.
Example: "xmodel/tree_river_birch_lg_a" -> "tree_river_birch_lg_a"
================
*/
void FS_ExtractBasename(const char *path, char *out)
{
  const char *p;

  /* find last path separator */
  for ( p = path + strlen(path) - 1; p != path; p-- )
  {
    if ( *(p - 1) == '/' || *(p - 1) == '\\' )
      break;
  }

  /* copy basename to output */
  if ( *p )
    strcpy(out, p);
  else
    *out = '\0';
}

/*
================
FS_GetPreviousPathComponent

Walks backwards to find the previous '/' delimited component.
Given path="a/b/c/d" and pos pointing at "c/d", returns pointer to "b/c/d".
================
*/
char *FS_GetPreviousPathComponent(const char *path, char *pos)
{
  char *p;

  Assert(path, s_assertDisable_FS_GetPreviousPathComponent);
  if ( !pos )
    return NULL;

  /* pos must be within or at end of path */
  Assert(pos >= path, g_fsPathFlags3);

  /* pos must be at string end, at path start, or right after a '/' */
  if ( *pos )
  {
    if ( pos == path )
      return NULL;
    Assert(*(pos - 1) == '/', g_fsPathFlags2);
  }

  if ( pos == path )
    return NULL;

  /* walk backwards to previous '/' */
  for ( p = pos - 1; p != path && *(p - 1) != '/'; p-- )
    ;
  return p;
}

/*
================
I_FloatTime

Returns current time as a double.
================
*/
double I_FloatTime(void)
{
  static DWORD baseTime;

  if ( !baseTime )
    baseTime = timeGetTime();

  return (double)(timeGetTime() - baseTime) * 0.001;
}

/*
================
Q_getwd

Gets current working directory with trailing '/' and backslash-to-slash conversion.
================
*/
void Q_getwd(char *buf)
{
  char *p;

  _getcwd(buf, MAX_OS_PATH_SHORT);

  /* append trailing slash */
  p = buf + strlen(buf);
  *p++ = '/';
  *p = '\0';

  /* convert backslashes to forward slashes */
  for ( p = buf; *p; p++ )
  {
    if ( *p == '\\' )
      *p = '/';
  }
}

/*
================
StripExtension

Removes the file extension from a path string.
================
*/
void StripExtension(char *path)
{
  int length;

  length = strlen(path) - 1;
  while ( length > 0 && path[length] != '.' )
  {
    length--;
    if ( path[length] == '/' )
      return;
  }
  if ( length )
    path[length] = 0;
}

/*
================
ExpandArg

Converts a relative path to absolute by prepending the current working directory.
================
*/
char *ExpandArg(const char *path)
{
  /* already absolute */
  if ( *path == '/' || *path == '\\' || path[1] == ':' )
  {
    strcpy(DstBuf, path);
    return DstBuf;
  }

  /* prepend current directory */
  Q_getwd(DstBuf);
  strcat(DstBuf, path);
  return DstBuf;
}

size_t fread_locked(void *Buffer, size_t ElementSize, size_t ElementCount, FILE *Stream)
{
  size_t bytesRead;
  _lock_file(Stream);
  bytesRead = fread(Buffer, ElementSize, ElementCount, Stream);
  _unlock_file(Stream);
  return bytesRead;
}

size_t fwrite_locked(void *Buffer, size_t ElementSize, size_t ElementCount, FILE *Stream)
{
  size_t bytesWritten;
  _lock_file(Stream);
  bytesWritten = fwrite(Buffer, ElementSize, ElementCount, Stream);
  _unlock_file(Stream);
  return bytesWritten;
}

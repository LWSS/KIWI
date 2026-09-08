/*
com_files.c — File system

Reconstructed from cod2map.exe by Rose.
*/

#include "cod4map.h"

Dvar_t           *fs_basegame;
Dvar_t           *fs_basepath = 0;
Dvar_t           *fs_cdpath;
Dvar_t           *fs_debug;
Dvar_t           *fs_gameDir_storage; Dvar_t **fs_gameDirVar = &fs_gameDir_storage;
Dvar_t           *fs_homepath;
Dvar_t           *fs_ignoreLocalized;
Dvar_t           *fs_useOldAssets;
FileHandleData_t  fsh[MAX_FILE_HANDLES];
Searchpath_t     *fs_searchpaths;

char          DstBuf[MAX_OS_PATH];
int           fs_loadStack;
char          g_assertLangBuf[138];
char          g_basePath[MAX_OS_PATH];
void        (*g_errorHandler)(const char *, va_list);
char          g_fsBaseGame[MAX_OS_PATH_SHORT] = "";
const char   *g_fsDefaultCfgDvar;
char          g_fsGameDir[MAX_OS_PATH_SHORT];
int           g_fsInitialized;
char          g_fsPathFlags0;
char          g_fsPathFlags1;
char          g_fsPathFlags2;
char          g_fsPathFlags3;
char          g_gameDirRelative[MAX_OS_PATH];
char          g_gameDir[MAX_OS_PATH];
int           g_langBufToggle;
char          g_ospath[MAX_OS_PATH];
char          g_savedFsBasepath[MAX_OS_PATH_SHORT];
char          g_savedFsGame[MAX_OS_PATH_SHORT];
int           g_totalIwdFiles;
static char   iwd_wildcard[] = "*.iwd";

char s_assertDisable_FS_HandleForFile;
char s_assertDisable_FS_FileForHandle;
char s_assertDisable_FS_FileForHandle;
char s_assertDisable_FS_FileForHandle;
char s_assertDisable_FS_filelength;
char s_assertDisable_FS_FCloseFile;
char s_assertDisable_FS_FOpenFileAppend;
char s_assertDisable_FS_SanitizePath;
char s_assertDisable_FS_BuildOSPathFull;
char s_assertDisable_FS_BuildOSPathFull;
char s_assertDisable_FS_BuildOSPathFull;
char s_assertDisable_FS_FOpenFileRead;
char s_assertDisable_FS_FOpenFileRead;
char s_assertDisable_FS_SV_GetFilepath;
char s_assertDisable_FS_SV_GetFilepath;
char s_assertDisable_FS_ListFilteredFiles;
char s_assertDisable_FS_ListFilteredFiles;
char s_assertDisable_FS_ListFiles;
char s_assertDisable_FS_AddGameDirectory;
char s_assertDisable_FS_InitFilesystem;

/*
================
FS_BuildOSPath

Builds a full OS filesystem path from a relative game path.
If the input is already an absolute path (starts with / or \ or has :),
it's returned as-is. Otherwise, the basepath is prepended.
Returns pointer to static buffer - not thread-safe.
================
*/
char *FS_BuildOSPath(char *relativePath)
{
  if ( !g_basePath[0] )
    Com_Error("ExpandPath called without install dir set");

  /* already absolute path */
  if ( *relativePath == '/' || *relativePath == '\\' || relativePath[1] == ':' )
  {
    strcpy(g_ospath, relativePath);
    return g_ospath;
  }

  /* prepend basepath */
  sprintf(g_ospath, "%s%s", g_basePath, relativePath);
  return g_ospath;
}

/*
================
FS_Startup

Initializes filesystem paths from map file location.
Extracts basepath and gamedir from the map path by finding the "maps" directory component.
================
*/
unsigned int FS_Startup(const char *mapPath)
{
  int pathLen;
  char *pos;
  char *installParent;
  char fullPath[MAX_OS_PATH];

  pathLen = GetFullPathNameA(mapPath, MAX_OS_PATH, fullPath, NULL);
  if ( pathLen <= 0 || pathLen >= MAX_OS_PATH )
    Com_Error("couldn't get full path for '%s'\n", mapPath);

  /* convert backslashes to forward slashes */
  for ( int i = 0; i < pathLen; i++ )
  {
    if ( fullPath[i] == '\\' )
      fullPath[i] = '/';
  }

  /* search backwards for "maps" directory component */
  pos = fullPath + strlen(fullPath);
  while ( 1 )
  {
    pos = FS_GetPreviousPathComponent(fullPath, pos);
    if ( !pos )
      Com_Error("No '%s' in '%s'\n", "maps", fullPath);
    if ( !_strnicmp(pos, "maps", 4) && (pos[4] == '\0' || pos[4] == '/') )
      break;
  }

  installParent = FS_GetPreviousPathComponent(fullPath, pos);
  if ( !installParent || installParent == fullPath )
    Com_Error("There should be two folders below '%s' in a proper install\n", "maps");

  memcpy(g_basePath, fullPath, installParent - fullPath);
  g_basePath[installParent - fullPath] = '\0';

  /* Shared assets are rooted at the target platform's install directory. */
  pos = FS_GetPreviousPathComponent(g_basePath, g_basePath + (installParent - fullPath));
  if ( pos && !_stricmp(pos, "share/") )
  {
    strcpy(g_basePath, g_targetPlatform->basePath);
    strcat(g_basePath, "/");
  }

  return Com_DPrintf("install directory: %s\n", g_basePath);
}

/*
================
FS_ReplacePlatformPath

Replaces platform-specific path component with substitute.
Scans path backwards for searchStr, replaces the matching component with replaceStr.
================
*/
char *FS_ReplacePlatformPath(char *path, char *searchStr, const char *replaceStr)
{
  int searchLen;
  int pathLen;
  char *scanPtr;
  const char *afterMatch;
  unsigned int i;
  char tempBuf[MAX_OS_PATH];

  searchLen = (int)strlen(searchStr);
  pathLen = (int)strlen(path);
  if (!searchLen || !pathLen)
    return path;

  /* An index keeps the empty path and the first character in bounds. */
  for (pathLen--; pathLen >= 0; --pathLen)
  {
    scanPtr = path + pathLen;
    if (Q_stricmpn(scanPtr, searchStr, searchLen))
      continue;
    for (afterMatch = scanPtr + searchLen; *afterMatch; ++afterMatch)
      if (*afterMatch == '/' || *afterMatch == '\\')
        break;
    if ((size_t)pathLen + strlen(replaceStr) + strlen(afterMatch) >= sizeof(tempBuf))
      Com_Error("FS_ReplacePlatformPath: path too long");
    memcpy(tempBuf, path, pathLen);
    tempBuf[pathLen] = 0;
    strcat(tempBuf, replaceStr);
    strcat(tempBuf, afterMatch);
    for (i = 0; tempBuf[i] && i < strlen(g_basePath); ++i)
      if (tempBuf[i] == '\\')
        tempBuf[i] = '/';
    strcpy(path, tempBuf);
  }

  return path;
}

/*
================
FS_LoadStack

Returns current file load stack depth.
================
*/
int FS_LoadStack(void)
{
  return fs_loadStack;
}

/*
================
FS_HashFileName

Computes hash of filename for hash table lookup.
================
*/
int FS_HashFileName(const char *fname, int hashSize)
{
  const char *p;
  int hash;
  int letter;

  /* position-weighted hash */
  hash = 0;
  for ( p = fname; p[0]; p++ )
  {
    letter = tolower(p[0]);
    if ( letter == '.' )
      break;
    if ( letter == '\\' )
      letter = '/';
    hash += letter * ((int)(p - fname) + HASH_POSITION_SEED);
  }
  return (hashSize - 1) & (hash ^ ((hash ^ (hash >> 10)) >> 10));
}

/*
================
FS_HandleForFile

Allocates a file handle slot.
================
*/
int FS_HandleForFile( int thread )
{
  int first, count, i;

  if ( thread )
  {
    if ( thread == FS_THREAD_STREAM )
    {
      first = FIRST_STREAM_HANDLE;
      count = NUM_STREAM_HANDLES;
    }
    else if ( thread == FS_THREAD_BACKEND )
    {
      first = FIRST_BACKEND_HANDLE;
      count = 1;
    }
    else
    {
      Assert( thread == FS_THREAD_DATABASE, s_assertDisable_FS_HandleForFile );
      Assert( Sys_IsMainThread(), s_assertDisable_FS_HandleForFile );
      first = FIRST_DATABASE_HANDLE;
      count = 1;
    }
  }
  else
  {
    Assert( Sys_IsMainThread(), s_assertDisable_FS_HandleForFile );
    first = FIRST_NORMAL_HANDLE;
    count = NUM_NORMAL_HANDLES;
  }

  /* find a free handle slot */
  for ( i = 0; i < count; i++ )
  {
    if ( !fsh[first + i].handleFile )
      return first + i;
  }

  if ( thread == FS_THREAD_MAIN )
  {
    for ( i = 1; i < MAX_FILE_HANDLES; i++ )
      Sys_Printf( "FILE %2i: '%s' 0x%x\n", i, fsh[i].name, fsh[i].handleFile );
    Com_ErrorLevel( 1, "FS_HandleForFile: none free" );
  }

  Sys_Printf( "FILE %2i: '%s' 0x%x\n", first, fsh[first].name, fsh[first].handleFile );
  Sys_Printf( "FS_HandleForFile: none free (%d)\n", thread );
  return 0;
}

/*
================
FS_FileForHandle

Validates file handle and returns FILE* pointer.
================
*/
void *FS_FileForHandle( int f )
{
  Assert(f > 0 && f < MAX_FILE_HANDLES, s_assertDisable_FS_FileForHandle);
  Assert( !fsh[f].zipFile, s_assertDisable_FS_FileForHandle );
  Assert( fsh[f].handleFile, s_assertDisable_FS_FileForHandle );
  return fsh[f].handleFile;
}

/*
================
FS_filelength

Returns length of an open file.
================
*/
int FS_filelength(int f)
{
  FILE *h;
  int pos;
  int end;

  Assert(f, s_assertDisable_FS_filelength);
  if ( !fs_searchpaths )
    Com_ErrorLevel(0, "Filesystem call made without initialization\n");
  if ( fsh[f].zipFile )
    return (int)((unz_s *)fsh[f].handleFile)->cur_file_info.uncompressed_size;
  h = FS_FileForHandle(f);
  pos = ftell(h);
  fseek(h, 0, SEEK_END);
  end = ftell(h);
  fseek(h, pos, SEEK_SET);
  return end;
}

/*
================
FS_ReplaceSeparators

Collapses duplicate path separators and normalizes to '\\'.
================
*/
char *FS_ReplaceSeparators(char *path)
{
  char wasSep;
  char *src;
  char *dst;
  char c;

  wasSep = 0;
  dst = path;
  src = path;
  while ( *src )
  {
    c = *src;
    if ( c == '/' || c == '\\' )
    {
      if ( !wasSep )
      {
        wasSep = 1;
        *dst++ = '\\';
      }
    }
    else
    {
      wasSep = 0;
      *dst++ = c;
    }
    ++src;
  }
  *dst = 0;
  return dst;
}

/*
================
FS_CreatePath

Creates directory structure for a path.
================
*/
int FS_CreatePath( char *OSPath )
{
  char *ofs;

  if ( strstr( OSPath, ".." ) || strstr( OSPath, "::" ) )
  {
    Sys_Printf( "WARNING: refusing to create relative path \"%s\"\n", OSPath );
    return 1;
  }

  /* create each directory component */
  for ( ofs = OSPath + 1; *ofs; ofs++ )
  {
    if ( *ofs == '\\' )
    {
      *ofs = 0;
      Sys_Mkdir( OSPath );
      *ofs = '\\';
    }
  }
  return 0;
}

/*
================
FS_CopyFile

Binary copies a file from source to destination.
================
*/
void FS_CopyFile(char *fromOSPath, char *toOSPath)
{
  FILE *f;
  size_t len;
  void *buf;
  FILE *f2;

  f = fopen(fromOSPath, "rb");
  if ( f )
  {
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(len);
    if ( fread_locked(buf, 1, len, f) != len )
      Com_ErrorLevel(0, "Short read in FS_CopyFile()\n");
    fclose(f);
    if ( !FS_CreatePath(toOSPath) && (f2 = fopen(toOSPath, "wb")) != 0 )
    {
      if ( fwrite_locked(buf, 1, len, f2) != len )
        Com_ErrorLevel(0, "Short write in FS_CopyFile()\n");
      fclose(f2);
      free(buf);
    }
    else
    {
      free(buf);
    }
  }
}

/*
================
FS_FCloseFile

Closes an open file handle.
================
*/
int FS_FCloseFile( int f )
{
  if ( !fs_searchpaths )
    Com_ErrorLevel( 0, "Filesystem call made without initialization\n" );

  Assert( !fsh[f].streamed, s_assertDisable_FS_FCloseFile );

  if ( fsh[f].zipFile )
  {
    unzCloseCurrentFile( fsh[f].handleFile );
    if ( fsh[f].uniqueFile )
      unzClose( fsh[f].handleFile );
    else
    {
      Iwd_t *iwd = (Iwd_t *)fsh[f].zipFile;
      Assert( iwd->hasOpenFile, s_assertDisable_FS_FCloseFile );
      iwd->hasOpenFile = 0;
    }
  }
  else if ( f )
  {
    fclose( (FILE *)FS_FileForHandle( f ) );
  }

  return Com_Memset( &fsh[f], 0, sizeof(FileHandleData_t) );
}

/*
================
FS_HandleOpen

Opens a file and allocates a handle slot.
================
*/
int FS_HandleOpen(const char *qpath, const char *ospath, int thread)
{
  FILE *fp;
  int f;

  fp = fopen(ospath, "wb");
  if ( !fp )
    return 0;

  f = FS_HandleForFile(thread);
  fsh[f].zipFile = 0;
  fsh[f].handleFile = fp;
  I_strncpyz(fsh[f].name, qpath, MAX_OS_PATH_SHORT);
  fsh[f].handleSync = 0;
  return f;
}

/*
================
FS_FilenameCompare

Case-insensitive path comparison (0=equal, -1=different).
================
*/
int FS_FilenameCompare(const char *s1, const char *s2)
{
  int c1, c2;

  do
  {
    c1 = *s1++;
    c2 = *s2++;
    if ( Q_islower(c1) )
      c1 -= ('a' - 'A');
    if ( Q_islower(c2) )
      c2 -= ('a' - 'A');
    if ( c1 == '\\' || c1 == ':' )
      c1 = '/';
    if ( c2 == '\\' || c2 == ':' )
      c2 = '/';
    if ( c1 != c2 )
      return -1;
  } while ( c1 );
  return 0;
}

/*
================
FS_SanitizePath

Sanitizes filename/path, rejects ".." and "::" sequences.
================
*/
char FS_SanitizePath( const char *filename, char *sanitizedName, int sanitizedNameSize )
{
  int src, dst;
  char c, peek;

  Assert( filename, g_assertLangBuf[3] );
  Assert( sanitizedName, g_assertLangBuf[2] );
  Assert(sanitizedNameSize > 0, g_assertLangBuf[1]);

  /* skip leading path separators */
  for ( src = 0; filename[src] == '/' || filename[src] == '\\'; src++ )
    ;

  dst = 0;
  while ( filename[src] )
  {
    /* reject ".." and "::" sequences */
    if ( filename[src] == '.' && filename[src + 1] == '.' )
      return 0;
    if ( filename[src] == ':' && filename[src + 1] == ':' )
      return 0;

    /* skip lone "." path components (e.g., "./foo" -> "foo") */
    peek = filename[src + 1];
    if ( filename[src] == '.' && (peek == 0 || peek == '/' || peek == '\\') )
    {
      src++;
      continue;
    }

    Assert(dst + 1 < sanitizedNameSize, g_assertLangBuf[0]);

    /* normalize separators to '/' and collapse consecutive ones */
    c = filename[src];
    if ( c == '/' || c == '\\' )
    {
      sanitizedName[dst++] = '/';
      while ( filename[src + 1] == '/' || filename[src + 1] == '\\' )
        src++;
    }
    else
    {
      sanitizedName[dst++] = c;
    }
    src++;
  }

  Assert(dst <= src, s_assertDisable_FS_SanitizePath);

  sanitizedName[dst] = 0;
  return 1;
}

/*
================
FS_Read

Reads data from an open file handle.
================
*/
int FS_Read( void *buffer, int len, int f )
{
  char *buf;
  FILE *h;
  int remaining, bytesRead, tries;

  if ( !fs_searchpaths )
    Com_ErrorLevel( 0, "Filesystem call made without initialization\n" );
  if ( !f )
    return 0;

  /* zip file: delegate to minizip */
  if ( fsh[f].zipFile )
    return unzReadCurrentFile( fsh[f].handleFile, buffer, len );

  buf = (char *)buffer;
  h = (FILE *)FS_FileForHandle( f );
  remaining = len;
  tries = 0;

  while ( remaining > 0 )
  {
    bytesRead = (int)fread_locked( buf, 1, remaining, h );
    if ( bytesRead == -1 )
    {
      /* stream handles return -1 on error instead of fatal */
      if ( f >= FIRST_STREAM_HANDLE && f < FIRST_DATABASE_HANDLE )
        return -1;
      Com_ErrorLevel( 0, "FS_Read: -1 bytes read" );
    }
    if ( !bytesRead )
    {
      if ( tries )
        return len - remaining;
      tries = 1;
    }
    remaining -= bytesRead;
    buf += bytesRead;
  }

  return len;
}

/*
================
FS_Write

Writes data to an open file handle.
================
*/
int FS_Write( const void *buffer, int len, int f )
{
  char *buf;
  FILE *h;
  int remaining, written, tries;

  if ( !fs_searchpaths )
    Com_ErrorLevel( 0, "Filesystem call made without initialization\n" );
  if ( !f )
    return 0;

  h = (FILE *)FS_FileForHandle( f );
  buf = (char *)buffer;
  remaining = len;
  tries = 0;

  while ( remaining > 0 )
  {
    written = (int)fwrite_locked( buf, 1, remaining, h );
    if ( !written )
    {
      if ( tries )
        return 0;
      tries = 1;
    }
    else if ( written == -1 )
      return 0;
    remaining -= written;
    buf += written;
  }

  if ( fsh[f].handleSync )
    fflush( h );
  return len;
}

/*
================
FS_ClearLoadStack

Resets the load stack counter.
================
*/
void FS_ClearLoadStack()
{
  fs_loadStack = 0;
}

/*
================
FS_FreeFile

Frees a file buffer allocated by FS_LoadFile.
================
*/
void FS_FreeFile( void *buffer )
{
  if ( !fs_searchpaths )
    Com_ErrorLevel( 0, "Filesystem call made without initialization\n" );
  Assert( buffer, g_assertLangBuf[7] );
  --fs_loadStack;
  Hunk_FreeTempMemory( buffer );
}

/*
================
FS_ReturnPath

Extracts directory path from a full filepath, returns last separator index.
================
*/
int FS_ReturnPath(char *zpath, int *depth, char *zname)
{
  int i, len, newdep;

  strcpy(zpath, zname);

  len = 0;
  newdep = 0;
  for ( i = 0; zname[i]; i++ )
  {
    if ( zname[i] == '/' || zname[i] == '\\' )
    {
      len = i;
      newdep++;
    }
  }
  zpath[len] = 0;

  /* trailing separator doesn't count as depth */
  if ( len + 1 == i )
    newdep--;
  *depth = newdep;
  return len;
}

/*
================
FS_AddFileToList

Adds filename to list if not already present (deduplication).
================
*/
int FS_AddFileToList(char **list, int nfiles, char *filename)
{
  int i;

  if ( nfiles == MAX_FOUND_FILES - 1 )
    return nfiles;
  for ( i = 0; i < nfiles; ++i )
  {
    if ( !Q_stricmp(filename, list[i]) )
      return nfiles;
  }
  list[nfiles] = CopyStringHunk(filename);
  return nfiles + 1;
}

/*
================
FS_IsGameDirAllowed

Checks if gamedir is allowed by the given flags bitmask.
================
*/
qboolean FS_IsGameDirAllowed(int flags, char *gamedir)
{
  #define FS_ALLOW_ALL    0x3F
  #define FS_ALLOW_MAIN   0x01
  #define FS_ALLOW_BASE   0x02
  #define FS_ALLOW_TEMP   0x04
  #define FS_ALLOW_RAW    0x08
  #define FS_ALLOW_RAWSH  0x10
  #define FS_ALLOW_DEVRAW 0x20
  return flags == FS_ALLOW_ALL
      || (flags & FS_ALLOW_MAIN)   && !Q_strncmp(gamedir, "main", 4)
      || (flags & FS_ALLOW_BASE)   && !Q_strncmp(gamedir, "dev", 3)
      || (flags & FS_ALLOW_TEMP)   && !Q_strncmp(gamedir, "temp", 4)
      || (flags & FS_ALLOW_RAW)    && !Q_strncmp(gamedir, "raw", 3)
      || (flags & FS_ALLOW_RAWSH)  && !Q_strncmp(gamedir, "raw_shared", 10)
      || (flags & FS_ALLOW_DEVRAW) && !Q_strncmp(gamedir, "devraw", 6);
  #undef FS_ALLOW_ALL
  #undef FS_ALLOW_MAIN
  #undef FS_ALLOW_BASE
  #undef FS_ALLOW_TEMP
  #undef FS_ALLOW_RAW
  #undef FS_ALLOW_RAWSH
  #undef FS_ALLOW_DEVRAW
}

/*
================
FS_PathCmp_ThreeWay

Three-way path comparison (-1, 0, 1).
================
*/
int FS_PathCmp_ThreeWay(const char *s1, const char *s2)
{
  int c1, c2;

  do
  {
    c1 = *s1++;
    c2 = *s2++;
    if ( Q_islower(c1) )
      c1 -= ('a' - 'A');
    if ( Q_islower(c2) )
      c2 -= ('a' - 'A');
    if ( c1 == '\\' || c1 == ':' )
      c1 = '/';
    if ( c2 == '\\' || c2 == ':' )
      c2 = '/';
    if ( c1 < c2 )
      return -1;
    if ( c1 > c2 )
      return 1;
  } while ( c1 );
  return 0;
}

/*
================
FS_DisplayPath

Prints current search paths and open file handles.
================
*/
int FS_DisplayPath( int bLanguageCull )
{
  Searchpath_t *s;
  int i;

  if ( fs_ignoreLocalized->current.enabled )
    Sys_Printf( "    localized assets are being ignored\n" );

  Sys_Printf( "Current search path:\n" );
  for ( s = (Searchpath_t *)fs_searchpaths; s; s = s->next )
  {
    if ( bLanguageCull && s->localized && fs_ignoreLocalized->current.enabled )
      continue;
    if ( s->iwd )
      Sys_Printf( "%s (%i files)\n", s->iwd, s->iwd->numFiles );
    else
      Sys_Printf( "%s/%s\n", s->dir, s->dir->gamedir );
  }

  Sys_Printf( "\nFile Handles:\n" );
  for ( i = 1; i < MAX_FILE_HANDLES; i++ )
  {
    if ( fsh[i].handleFile )
      Sys_Printf( "handle %i: %s\n", i, fsh[i].name );
  }
  return 0;
}

/*
================
IwdFileLanguage

Extracts language string from IWD filename (e.g. "localized_english_iw00").
================
*/
char *IwdFileLanguage(const char *iwdName)
{
  #define LANG_BUF_OFFSET 10
  #define LANG_BUF_SIZE   64
  char *buf;
  int slot, i;

  slot = g_langBufToggle ^ 1;
  g_langBufToggle = slot;
  buf = &g_assertLangBuf[LANG_BUF_OFFSET + LANG_BUF_SIZE * slot];

  if ( strlen(iwdName) < LOCALIZED_PREFIX_LEN )
  {
    buf[0] = '\0';
    return buf;
  }

  memset(buf, 0, LANG_BUF_SIZE);
  for ( i = LOCALIZED_PREFIX_LEN; i < LANG_BUF_SIZE; i++ )
  {
    if ( !iwdName[i] || !isalpha(iwdName[i]) )
      break;
    buf[i - LOCALIZED_PREFIX_LEN] = iwdName[i];
  }
  return buf;
  #undef LANG_BUF_OFFSET
  #undef LANG_BUF_SIZE
}

/*
================
iwdsort

qsort comparator for IWD files — english locale gets priority.
================
*/
int iwdsort(char **elem1, char **elem2)
{
  char *name2;
  char *name1;
  char *lang1;
  char *lang2;

  name2 = *elem2;
  name1 = *elem1;
  if ( !Q_strncmp(*elem1, g_localizedBlankPrefix, LOCALIZED_PREFIX_LEN) && !Q_strncmp(name2, g_localizedBlankPrefix, LOCALIZED_PREFIX_LEN) )
  {
    lang2 = IwdFileLanguage(name1);
    lang1 = IwdFileLanguage(name2);
    if ( Q_stricmp(lang2, "english") )
    {
      if ( !Q_stricmp(lang1, "english") )
        return 1;
    }
    else if ( Q_stricmp(lang1, "english") )
    {
      return -1;
    }
  }
  return FS_PathCmp_ThreeWay(name1, name2);
}

/*
================
FS_FreeSearchPaths

Frees a linked list of search path entries.
================
*/
void FS_FreeSearchPaths( void *searchPath )
{
  Searchpath_t *cur, *next;

  for ( cur = (Searchpath_t *)searchPath; cur; cur = next )
  {
    next = cur->next;
    if ( cur->iwd )
    {
      unzClose( cur->iwd->handle );
      Z_Free( cur->iwd->buildBuffer );
      Z_Free( cur->iwd );
    }
    if ( cur->dir )
      Z_Free( cur->dir );
    Z_Free( cur );
  }
}

/*
================
FS_Shutdown

Closes all file handles and frees all search paths.
================
*/
void FS_Shutdown( int closemfp )
{
  int i;

  for ( i = 1; i < MAX_FILE_HANDLES; i++ )
  {
    if ( fsh[i].fileSize )
      FS_FCloseFile( i );
  }
  FS_FreeSearchPaths( (void *)fs_searchpaths );
  fs_searchpaths = 0;
}

/*
================
FS_RegisterDvars

Registers filesystem dvars (fs_debug, fs_basepath, etc.).
================
*/
static bool __cdecl FS_GameDirDomain(Dvar_t *dvar, DvarValue_t value)
{
  const char *gameDir;

  Assert(dvar != NULL, s_assertDisable_FS_InitFilesystem);
  gameDir = value.string;
  if ( !gameDir[0] )
    return true;
  if ( !Q_stricmpn(gameDir, "mods", 4)
    && strlen(gameDir) >= 6
    && (gameDir[4] == '/' || gameDir[4] == '\\')
    && !strstr(gameDir, "..")
    && !strstr(gameDir, "::") )
  {
    return true;
  }

  Com_ErrorLevel(1, "ERROR: Invalid server value '%s' for '%s'\n\n", gameDir, dvar->name);
  return false;
}

char FS_RegisterDvars()
{
  const char *cdPath;
  char *basePath;
  char *homePath;
  char normalizedGameDir[260];
  int i;
  bool changed;

  fs_debug = Dvar_RegisterInt("fs_debug", 0, 0, 2, DVAR_NOFLAG, NULL);
  fs_copyfiles = Dvar_RegisterBool("fs_copyfiles", 0, DVAR_INIT,
                                  "Copy all used files to another location");
  cdPath = Sys_DefaultCDPath();
  fs_cdpath = Dvar_RegisterString("fs_cdpath", cdPath, DVAR_INIT, NULL);
  basePath = Sys_DefaultBasePath();
  fs_basepath = Dvar_RegisterString("fs_basepath", basePath, (DVAR_INIT | DVAR_SAVED), NULL);
  fs_basegame = Dvar_RegisterString("fs_basegame", g_fsBaseGame, DVAR_INIT, NULL);
  *fs_gameDirVar = Dvar_RegisterString("fs_game", g_fsBaseGame,
                                      (DVAR_SERVERINFO | DVAR_SYSTEMINFO | DVAR_INIT), NULL);
  Dvar_SetDomainFunc(*fs_gameDirVar, FS_GameDirDomain);

  Assert(((*fs_gameDirVar)->flags & DVAR_INIT) != 0, s_assertDisable_FS_InitFilesystem);
  I_strncpyz(normalizedGameDir, (*fs_gameDirVar)->current.string, sizeof(normalizedGameDir));
  changed = false;
  for ( i = 0; normalizedGameDir[i]; ++i )
  {
    if ( normalizedGameDir[i] == '\\' )
    {
      normalizedGameDir[i] = '/';
      changed = true;
    }
  }
  if ( changed )
    Dvar_SetString(*fs_gameDirVar, normalizedGameDir);

  fs_ignoreLocalized = Dvar_RegisterBool("fs_ignoreLocalized", 0, (DVAR_LATCH | DVAR_CHEAT),
                                        "Ignore localized files");
  homePath = (char *)Sys_DefaultHomePath();
  if ( !homePath || !*homePath )
    homePath = (char *)fs_basepath->reset.string;
  fs_homepath = Dvar_RegisterString("fs_homepath", homePath, DVAR_INIT, NULL);
  return 1;
}

/*
================
FS_BuildOSPathFull

Builds "base/gamedir/qpath" into ospath buffer.
================
*/
char *FS_BuildOSPathFull( const char *base, char *gamedir, const char *qpath, char *ospath, char *lenCheck )
{
  unsigned int baseLen, dirLen, qpathLen;

  Assert( base, s_assertDisable_FS_BuildOSPathFull );
  Assert( qpath, s_assertDisable_FS_BuildOSPathFull );
  Assert( ospath, s_assertDisable_FS_BuildOSPathFull );

  if ( !gamedir || !*gamedir )
    gamedir = g_fsGameDir;

  baseLen = (unsigned int)strlen( base );
  dirLen = (unsigned int)strlen( gamedir );
  qpathLen = (unsigned int)strlen( qpath );

  /* check total path length: base/gamedir/qpath */
  if ( (int)(baseLen + dirLen + qpathLen + 2) >= MAX_OS_PATH_SHORT )
  {
    if ( lenCheck )
    {
      *ospath = '\0';
      return lenCheck;
    }
    Com_ErrorLevel( 0, "FS_BuildOSPath: os path length exceeded\n" );
  }

  /* build "base/gamedir/qpath" */
  memcpy( ospath, base, baseLen );
  ospath[baseLen] = '/';
  memcpy( &ospath[baseLen + 1], gamedir, dirLen );
  ospath[baseLen + 1 + dirLen] = '/';
  memcpy( &ospath[baseLen + 2 + dirLen], qpath, qpathLen + 1 );

  return FS_ReplaceSeparators( ospath );
}

/*
================
FS_FOpenFileWrite

Opens a file for writing in the home path.
================
*/
int FS_FOpenFileWrite( char *qpath )
{
  char ospath[MAX_OS_PATH_SHORT];

  if ( !fs_searchpaths )
    Com_ErrorLevel( 0, "Filesystem call made without initialization\n" );
  FS_BuildOSPathFull( fs_homepath->current.string, g_fsGameDir, qpath, ospath, 0 );
  if ( fs_debug->current.integer )
    Sys_Printf( "FS_FOpenFileWrite: %s\n", ospath );
  if ( FS_CreatePath( ospath ) )
    return 0;
  return FS_HandleOpen( qpath, ospath, FS_THREAD_MAIN );
}

/*
================
FS_FOpenFileAppend

Opens a file for appending in the home path.
================
*/
int FS_FOpenFileAppend( char *qpath )
{
  int f;
  FILE *file;
  char ospath[MAX_OS_PATH_SHORT];

  Assert( Sys_IsMainThread() || Sys_IsRenderThread(), s_assertDisable_FS_FOpenFileAppend );
  if ( !fs_searchpaths )
    Com_ErrorLevel(0, "Filesystem call made without initialization\n");
  f = FS_HandleForFile(Sys_IsMainThread() ? FS_THREAD_MAIN : FS_THREAD_BACKEND);
  fsh[f].zipFile = 0;
  I_strncpyz(fsh[f].name, qpath, MAX_OS_PATH_SHORT);
  FS_BuildOSPathFull(fs_homepath->current.string, g_fsGameDir, qpath, ospath, 0);
  if ( fs_debug->current.integer )
    Sys_Printf("FS_FOpenFileAppend: %s\n", ospath);
  if ( FS_CreatePath(ospath) )
    return 0;
  file = fopen(ospath, "at");
  fsh[f].handleFile = file;
  fsh[f].handleSync = 0;
  if ( !file )
    return 0;
  return f;
}

/*
================
FS_IsExt

Checks if filename has a known text-based extension.
================
*/
char FS_IsExt(const char *filename)
{
  static const char *extEntries[] = {
    ".hlsl", ".txt", ".cfg", ".levelshots", ".menu", ".arena", ".str", ""
  };
  int i, nameLen, extLen;

  nameLen = (int)strlen(filename);
  for ( i = 0; *extEntries[i]; i++ )
  {
    extLen = (int)strlen(extEntries[i]);
    if ( !Q_stricmp(&filename[nameLen - extLen], extEntries[i]) )
      return 1;
  }
  return 0;
}

/*
================
FS_FOpenFileRead

Opens a file for reading across search paths, returns file size.
filename = game-relative path to open
thread = native filesystem thread class used for handle allocation and path checks
fileHandleOut = output file handle (if NULL, just checks existence)
================
*/
int IwdCompareExchangeHasOpenFile(int *value, int exchange, int comparand)
{
  int oldValue = *value;

  if ( oldValue == comparand )
    *value = exchange;
  return oldValue;
}

int FS_FOpenFileRead(const char *filename, int *handleOut, int thread)
{
  Searchpath_t *sp;
  Iwd_t *pak;
  Directory_t *dir;
  FileInIwd_t *entry;
  int f;
  char ospath[MAX_OS_PATH_SHORT];
  char sanitized[MAX_OS_PATH_SHORT];

  Assert(filename, s_assertDisable_FS_FOpenFileRead);
  if ( !fs_searchpaths )
    Com_ErrorLevel(0, "Filesystem call made without initialization\n");

  if ( !FS_SanitizePath(filename, sanitized, MAX_OS_PATH_SHORT) )
  {
    if ( handleOut )
      *handleOut = 0;
    return -1;
  }

  /* existence check only — no file handle needed */
  if ( !handleOut )
  {
    for ( sp = fs_searchpaths; sp; sp = sp->next )
    {
      if ( sp->localized && fs_ignoreLocalized->current.enabled )
        continue;

      pak = sp->iwd;
      if ( pak )
      {
        for ( entry = pak->hashTable[FS_HashFileName(sanitized, pak->hashSize)]; entry; entry = entry->next )
          if ( !FS_FilenameCompare(entry->name, sanitized) )
            return 1;
        continue;
      }

      dir = sp->dir;
      if ( dir )
      {
        FILE *fp;
        FS_BuildOSPathFull(dir->path, dir->gamedir, sanitized, ospath, thread ? (char *)1 : NULL);
        fp = fopen(ospath, "rb");
        if ( fp ) { fclose(fp); return 1; }
      }
    }
    return -1;
  }

  /* open for reading — allocate file handle and search */
  f = FS_HandleForFile(thread);
  *handleOut = f;
  if ( !f )
    return -1;

  for ( sp = fs_searchpaths; sp; sp = sp->next )
  {
    if ( sp->localized && fs_ignoreLocalized->current.enabled )
      continue;

    /* search in IWD/ZIP archives */
    pak = sp->iwd;
    if ( pak )
    {
      for ( entry = pak->hashTable[FS_HashFileName(sanitized, pak->hashSize)]; entry; entry = entry->next )
      {
        if ( FS_FilenameCompare(entry->name, sanitized) )
          continue;

        /* found in archive */
        if ( !pak->referenced && !FS_IsExt(sanitized) )
          pak->referenced = 1;

        if ( IwdCompareExchangeHasOpenFile(&pak->hasOpenFile, 1, 0) == 1 )
        {
          fsh[f].uniqueFile = 1;
          fsh[f].handleFile = unzReOpen(pak->iwdFilename, pak->handle);
          if ( !fsh[f].handleFile )
          {
            if ( thread ) { FS_FCloseFile(f); *handleOut = 0; return -1; }
            Com_ErrorLevel(0, "Couldn't reopen %s", pak->iwdFilename);
          }
        }
        else
        {
          fsh[f].uniqueFile = 0;
          fsh[f].handleFile = pak->handle;
        }

        I_strncpyz(fsh[f].name, sanitized, sizeof(fsh[f].name));
        fsh[f].zipFile = pak;
        {
          unz_s *zip = (unz_s *)fsh[f].handleFile;
          ZIP_FILE *file = zip->file;
          file_in_zip_read_info_s *readInfo = zip->pfile_in_zip_read;

          unzSetCurrentFileInfoPosition(pak->handle, entry->pos);
          Com_Memcpy(zip, pak->handle, sizeof(unz_s));
          zip->file = file;
          zip->pfile_in_zip_read = readInfo;
        }
        unzOpenCurrentFile(fsh[f].handleFile);
        fsh[f].zipFilePos = (intptr_t)entry->pos;

        if ( fs_debug->current.integer && !thread )
          Sys_Printf("FS_FOpenFileRead: %s (found in '%s')\n", sanitized, pak->iwdFilename);

        return (int)((unz_s *)fsh[f].handleFile)->cur_file_info.uncompressed_size;
      }
      continue;
    }

    /* search on disk */
    dir = sp->dir;
    if ( dir )
    {
      FS_BuildOSPathFull(dir->path, dir->gamedir, sanitized, ospath, thread ? (char *)1 : NULL);
      fsh[f].handleFile = fopen(ospath, "rb");
      if ( fsh[f].handleFile )
      {
        I_strncpyz(fsh[f].name, sanitized, sizeof(fsh[f].name));
        fsh[f].zipFile = 0;

        if ( fs_debug->current.integer && !thread )
          Sys_Printf("FS_FOpenFileRead: %s (found in '%s/%s')\n", sanitized, dir->path, dir->gamedir);

        if ( fs_copyfiles->current.enabled && !Q_stricmp(dir->path, fs_cdpath->current.string) )
        {
          char destPath[MAX_OS_PATH_SHORT];
          FS_BuildOSPathFull(fs_basepath->current.string, dir->gamedir, sanitized, destPath, thread ? (char *)1 : NULL);
          FS_CopyFile(ospath, destPath);
        }
        return FS_filelength(f);
      }
    }
  }

  if ( fs_debug->current.integer && !thread )
    Sys_Printf("Can't find %s\n", filename);
  *handleOut = 0;
  return -1;
}

/*
================
FS_Remove

Removes a file from the home path.
================
*/
qboolean FS_Remove( const char *qpath )
{
  char ospath[MAX_OS_PATH_SHORT];

  if ( !fs_searchpaths )
    Com_ErrorLevel( 0, "Filesystem call made without initialization\n" );
  Assert( qpath, s_assertDisable_FS_FOpenFileRead );
  if ( !*qpath )
    return 0;
  FS_BuildOSPathFull( fs_homepath->current.string, g_fsGameDir, qpath, ospath, 0 );
  return remove( ospath ) != -1;
}

/*
================
FS_LoadFile

Loads a file from the filesystem (IWD or disk) into a buffer.
================
*/
int FS_LoadFile(const char *filename, void **bufferptr)
{
  int fileSize;
  char *buffer;
  int fileHandle = 0;

  if ( !fs_searchpaths )
    Com_ErrorLevel(0, "Filesystem call made without initialization\n");
  if ( !filename || !*filename )
    Com_ErrorLevel(0, "FS_ReadFile with empty name\n");
  g_fsInitialized = 1;

  fileSize = FS_FOpenFileRead( filename, &fileHandle, FS_THREAD_MAIN );

  if ( !fileHandle )
  {
    if ( bufferptr )
      *bufferptr = 0;
    return -1;
  }

  if ( bufferptr )
  {
    ++fs_loadStack;
    buffer = (char *)Hunk_AllocateTempMemory( fileSize + 1 );
    *bufferptr = buffer;
    FS_Read( buffer, fileSize, fileHandle );
    buffer[fileSize] = 0;
  }
  FS_FCloseFile( fileHandle );
  return fileSize;
}

/*
================
FS_SV_GetFilepath

Finds a file across search paths, returns its OS path.
================
*/
int FS_SV_GetFilepath( const char *filename, char *ospath )
{
  Searchpath_t *sp;
  FILE *file;
  char sanitized[MAX_OS_PATH_SHORT];

  Assert( filename, s_assertDisable_FS_SV_GetFilepath );
  Assert( ospath, s_assertDisable_FS_SV_GetFilepath );

  if ( !FS_SanitizePath( filename, sanitized, MAX_OS_PATH_SHORT ) )
    return -1;

  /* search directory paths (skip IWD packs) */
  for ( sp = (Searchpath_t *)fs_searchpaths; sp; sp = sp->next )
  {
    if ( sp->localized && fs_ignoreLocalized->current.enabled )
      continue;
    if ( sp->iwd )
      continue;

    FS_BuildOSPathFull( sp->dir->path, sp->dir->gamedir, sanitized, ospath, 0 );
    file = fopen( ospath, "rb" );
    if ( file )
    {
      fclose( file );
      return 0;
    }
  }

  return -1;
}

/*
================
FS_LoadZipFile

Loads a zip/IWD file, builds pack info with hash table.
================
*/
Iwd_t *FS_LoadZipFile( char *zipPath, char *basename )
{
  unzFile uf;
  Iwd_t *pak;
  FileInIwd_t *fileArray, *entry;
  int *checksumArray, *checksumPtr;
  char *namePool;
  unsigned int numFiles, hashSize;
  size_t nameBytes;
  int i, hash;
  unz_file_info fileInfo;
  unz_global_info globalInfo;
  char nameBuf[MAX_OS_PATH_SHORT];

  uf = unzOpen( zipPath );
  if ( !uf )
    return NULL;
  if ( unzGetGlobalInfo( uf, &globalInfo ) )
    return NULL;
  numFiles = (unsigned int)globalInfo.number_entry;

  g_totalIwdFiles += numFiles;

  nameBytes = 0;
  unzGoToFirstFile( uf );
  for ( i = 0; i < (int)numFiles; ++i )
  {
    if ( unzGetCurrentFileInfo( uf, &fileInfo, nameBuf, MAX_OS_PATH_SHORT, NULL, 0, NULL, 0 ) )
      break;
    nameBytes += strlen(nameBuf) + 1;
    unzGoToNextFile( uf );
  }

  /* Native uses one exact-sized zeroed block for entries and names. */
  fileArray = (FileInIwd_t *)Z_Malloc( sizeof(FileInIwd_t) * numFiles + nameBytes );
  namePool = (char *)&fileArray[numFiles];

  checksumArray = (int *)Z_Malloc( sizeof(int) * numFiles );

  /* hash table size: next power of 2 >= numFiles, max 1024 */
  for ( hashSize = 1; hashSize <= MAX_IWD_HASH_SIZE && hashSize <= numFiles; hashSize *= 2 )
    ;

  /* allocate Iwd_t + inline hash table */
  pak = (Iwd_t *)Z_Malloc( sizeof(Iwd_t) + sizeof(FileInIwd_t *) * hashSize );
  pak->hashTable = (FileInIwd_t **)((char *)pak + sizeof(Iwd_t));
  pak->hashSize = hashSize;
  for ( i = 0; i < (int)hashSize; i++ )
    pak->hashTable[i] = NULL;

  I_strncpyz( pak->iwdFilename, zipPath, MAX_OS_PATH_SHORT );
  I_strncpyz( pak->iwdBasename, basename, MAX_OS_PATH_SHORT );

  /* strip .iwd extension from basename */
  if ( strlen( pak->iwdBasename ) > 4
    && !Q_stricmp( pak->iwdBasename + strlen( pak->iwdBasename ) - 4, ".iwd" ) )
    pak->iwdBasename[strlen( pak->iwdBasename ) - 4] = 0;

  pak->handle = uf;
  pak->numFiles = numFiles;
  unzGoToFirstFile( uf );

  /* read each file entry and insert into hash table */
  checksumPtr = checksumArray;
  for ( i = 0, entry = fileArray; i < (int)numFiles; i++, entry++ )
  {
    if ( unzGetCurrentFileInfo( uf, &fileInfo, nameBuf, MAX_OS_PATH_SHORT, NULL, 0, NULL, 0 ) )
      break;

    if ( fileInfo.uncompressed_size )
    {
      *checksumPtr = BigShort( fileInfo.crc );
      checksumPtr++;
    }

    Q_strlwr( nameBuf );
    hash = FS_HashFileName( nameBuf, pak->hashSize );
    entry->name = namePool;
    strcpy( namePool, nameBuf );
    namePool += strlen( nameBuf ) + 1;

    { unsigned long pos;
    unzGetCurrentFileInfoPosition( uf, &pos );
    entry->pos = (unsigned int)pos; }
    entry->next = pak->hashTable[hash];
    pak->hashTable[hash] = entry;
    unzGoToNextFile( uf );
  }

  Z_Free( checksumArray );
  pak->buildBuffer = fileArray;
  return pak;
}

/*
================
FS_ListFilteredFiles

Lists files matching path+extension across search paths.
================
*/
static void FS_AddOwnedFileToList(char **result, int *numFiles, char **hunkPos,
                                  char *hunkEnd, const char *filename)
{
  size_t copyLength;
  int listIndex;

  if ( *numFiles == 0x1FFF )
    return;
  for ( listIndex = 0; listIndex < *numFiles; ++listIndex )
  {
    if ( !Q_stricmp(filename, result[listIndex]) )
      return;
  }

  copyLength = strlen(filename) + 1;
  if ( copyLength > (size_t)(hunkEnd - *hunkPos) )
    Com_ErrorLevel(0, "Hunk_UserAlloc: out of memory");
  memcpy(*hunkPos, filename, copyLength);
  result[(*numFiles)++] = *hunkPos;
  *hunkPos += copyLength;
}

char **FS_ListFilteredFiles(Searchpath_t *searchPaths, const char *path, char *extension, char *filter, int flags, int *numFilesOut)
{
  Searchpath_t *sp;
  Iwd_t *pak;
  Directory_t *dir;
  FileInIwd_t *entry;
  char *name;
  int i, numFiles, pathLen, extLen, nameLen, pathDepth, subDepth, retLen, prefixLen, diskCount;
  int isSlashExt;
  void **diskList;
  void *hunkUser;
  char **result;
  char *hunkPos;
  char *hunkEnd;
  char pathBuf[MAX_OS_PATH_SHORT];
  char ospathBuf[MAX_OS_PATH_SHORT];
  char sanitizedPath[MAX_OS_PATH_SHORT];
  char directoryName[MAX_QPATH + 4];

  if ( !fs_searchpaths )
    Com_ErrorLevel(0, "Filesystem call made without initialization\n");
  if ( !path )
  {
    *numFilesOut = 0;
    return 0;
  }
  if ( !extension )
    extension = "";
  if ( !FS_SanitizePath(path, sanitizedPath, MAX_OS_PATH_SHORT) )
  {
    *numFilesOut = 0;
    return 0;
  }

  isSlashExt = Q_stricmp(extension, "/") == 0;
  pathLen = (int)strlen(sanitizedPath);
  if ( pathLen && (sanitizedPath[pathLen - 1] == '/' || sanitizedPath[pathLen - 1] == '\\') )
    pathLen--;
  extLen = (int)strlen(extension);

  FS_ReturnPath(pathBuf, &pathDepth, sanitizedPath);
  if ( sanitizedPath[0] )
    pathDepth++;

  /*
   * Native 0x46A1D0 owns both the pointer list and its copied names through
   * one HunkUser (0x20000 bytes in the x86 reference). The slot before the returned
   * list is the owner pointer; callers of the engine API use that to release
   * the whole list as one allocation.  Keep the same externally visible
   * layout while this standalone compiler retains its hunk-user carrier
   * locally.
   */
  /* Reserve pointer slots separately from the copied-name arena. */
  hunkUser = Z_Malloc(32 + sizeof(void *) + 0x2000 * sizeof(char *) + 0x18000);
  result = (char **)((char *)hunkUser + 32);
  result[0] = (char *)hunkUser;
  ++result;
  hunkPos = (char *)(result + 0x2000);
  hunkEnd = hunkPos + 0x18000;
  numFiles = 0;

  for ( sp = searchPaths; sp; sp = sp->next )
  {
    if ( sp->localized && fs_ignoreLocalized->current.enabled )
      continue;

    pak = sp->iwd;
    if ( pak )
    {
      /* search in IWD archive */
      for ( i = 0, entry = pak->buildBuffer; i < pak->numFiles; i++, entry++ )
      {
        name = entry->name;
        if ( filter )
        {
          if ( Com_FilterPath(filter, name, 0) )
            FS_AddOwnedFileToList(result, &numFiles, &hunkPos, hunkEnd, name);
          continue;
        }

        retLen = FS_ReturnPath(pathBuf, &subDepth, name);
        if ( subDepth != pathDepth || pathLen > retLen
          || (pathLen > 0 && name[pathLen] != '/')
          || Q_stricmpn(name, sanitizedPath, pathLen) )
          continue;

        if ( isSlashExt )
        {
          Assert(extLen == 1, s_assertDisable_FS_ListFilteredFiles);
          Assert(*extension == '/' && !extension[1], s_assertDisable_FS_ListFilteredFiles);
          if ( name[strlen(name) - 1] == '/' )
          {
            prefixLen = pathLen ? pathLen + 1 : 0;
            strcpy(directoryName, &name[prefixLen]);
            directoryName[strlen(directoryName) - 1] = 0;
            FS_AddOwnedFileToList(result, &numFiles, &hunkPos, hunkEnd, directoryName);
          }
          continue;
        }

        /* extension matching */
        if ( extLen )
        {
          nameLen = Q_strlen(name);
          if ( nameLen <= extLen || name[nameLen - extLen - 1] != '.'
            || Q_stricmp(&name[nameLen - extLen], extension) )
            continue;
        }

        prefixLen = pathLen ? pathLen + 1 : 0;
        FS_AddOwnedFileToList(result, &numFiles, &hunkPos, hunkEnd, &name[prefixLen]);
      }
    }
    else
    {
      /* search on disk */
      dir = sp->dir;
      if ( dir )
      {
        FS_BuildOSPathFull(dir->path, dir->gamedir, sanitizedPath, ospathBuf, 0);
        diskList = (void **)Sys_ListFiles(ospathBuf, extension, (const char *)filter, &diskCount, isSlashExt);
        for ( i = 0; i < diskCount; i++ )
          FS_AddOwnedFileToList(result, &numFiles, &hunkPos, hunkEnd, (char *)diskList[i]);
        Sys_FreeFileList((char **)diskList);
      }
    }
  }

  *numFilesOut = numFiles;
  if ( !numFiles )
  {
    Z_Free(hunkUser);
    return 0;
  }
  result[numFiles] = 0;
  return result;
}

/*
================
FS_FreeFileList

The filtered-list API stores its arena owner in the pointer slot before the list.
The native free wrapper recovers and destroys that owner as a single unit.
================
*/
void FS_FreeFileList(char **list)
{
  if ( list )
    Z_Free(list[-1]);
}

/*
================
FS_ListFiles

Builds filtered search path list and calls FS_ListFilteredFiles.
================
*/
char **FS_ListFiles( const char *path, char *extension, char *filter, int flags, int *numFilesOut, int gameDirFlags )
{
  Searchpath_t *sp, *head, *tail, *newNode, *next;
  char *dirName;
  char **result;

  /* build filtered search path list */
  head = NULL;
  tail = NULL;
  for ( sp = (Searchpath_t *)fs_searchpaths; sp; sp = sp->next )
  {
    if ( sp->dir )
      dirName = sp->dir->gamedir;
    else if ( sp->iwd )
      dirName = sp->iwd->iwdGamename;
    else
      dirName = NULL;

    Assert( dirName, s_assertDisable_FS_ListFiles );

    if ( !FS_IsGameDirAllowed( gameDirFlags, dirName ) )
      continue;

    newNode = Z_Malloc( sizeof(Searchpath_t) );
    newNode->next = NULL;
    newNode->dir = sp->dir;
    newNode->iwd = sp->iwd;
    newNode->ignore = sp->ignore;
    newNode->ignorePureCheck = sp->ignorePureCheck;
    newNode->language = sp->language;
    newNode->localized = sp->localized;

    if ( tail )
      tail->next = newNode;
    else
      head = newNode;
    tail = newNode;
  }

  result = FS_ListFilteredFiles( head, path, extension, filter, flags, numFilesOut );

  /* free filtered path list */
  for ( sp = head; sp; sp = next )
  {
    next = sp->next;
    Z_Free( sp );
  }

  return result;
}

/*
================
FS_AddIwdFilesForGameDirectory

Loads all .iwd files from basepath/gamedir.
================
*/
void FS_AddIwdFilesForGameDirectory( const char *basepath, char *gamedir )
{
  char *sorted[MAX_IWD_FILES];
  char **fileList;
  int numFiles, i, localized;
  char *curName, *lang;
  Iwd_t *pakFile;
  Searchpath_t *searchNode;
  Searchpath_t **insertPoint;
  char ospath[MAX_OS_PATH_SHORT];

  /* build OS path and list all .iwd files */
  FS_BuildOSPathFull( basepath, gamedir, "", ospath, NULL );
  ospath[strlen( ospath ) - 1] = 0;

  numFiles = 0;
  fileList = Sys_ListFiles( ospath, "iwd", 0, &numFiles, 0 );

  if ( numFiles > MAX_IWD_FILES )
  {
    Sys_Printf( "WARNING: Exceeded max number of iwd files in %s/%s (%i/%i)\n",
      basepath, gamedir, numFiles, MAX_IWD_FILES );
    numFiles = MAX_IWD_FILES;
  }

  /* first pass: copy pointers, replace "localized_" prefix with spaces for sorting */
  for ( i = 0; i < numFiles; i++ )
  {
    sorted[i] = (char *)fileList[i];
    if ( !Q_strncmp( sorted[i], "localized_", 10 ) )
      memcpy( sorted[i], g_localizedBlankPrefix, 10 );
  }

  qsort( sorted, numFiles, sizeof(char *), (int (*)(const void *, const void *))iwdsort );

  /* second pass: load each IWD */
  for ( i = 0; i < numFiles; i++ )
  {
    curName = sorted[i];

    /* check if this was a localized file (space prefix) */
    if ( !Q_strncmp( curName, g_localizedBlankPrefix, 10 ) )
    {
      memcpy( curName, "localized_", 10 );
      localized = 1;

      lang = IwdFileLanguage( curName );
      if ( !*lang )
      {
        Sys_Printf( "WARNING: Localized assets iwd file %s/%s/%s has invalid name (no language specified). Skipping.\n",
          basepath, gamedir, curName );
        continue;
      }
      if ( Q_stricmp( lang, "english" ) )
        continue;  /* not english — skip */
    }
    else
    {
      localized = 0;
    }

    /* build full OS path and load IWD */
    FS_BuildOSPathFull( basepath, gamedir, curName, ospath, NULL );
    pakFile = FS_LoadZipFile( ospath, curName );
    if ( !pakFile )
      continue;

    strcpy( pakFile->iwdGamename, gamedir );

    /* allocate search path node and insert into list */
    searchNode = Z_Malloc( sizeof(Searchpath_t) );
    searchNode->iwd = pakFile;
    searchNode->localized = localized;
    searchNode->language = 0;

    /* localized packs insert before first localized entry */
    insertPoint = &fs_searchpaths;
    if ( localized && fs_searchpaths )
    {
      Searchpath_t *cur;
      for ( cur = *insertPoint; cur; cur = cur->next )
      {
        if ( cur->localized )
          break;
        insertPoint = &cur->next;
      }
    }
    searchNode->next = *insertPoint;
    *insertPoint = searchNode;
  }

  /* free file list */
  Sys_FreeFileList( (char **)fileList );
}

/*
================
FS_AddGameDirectory

Adds a game directory to the search path.
================
*/
void FS_AddGameDirectory( const char *basepath, int localized, const char *gamedir, int language )
{
  Searchpath_t *sp, *searchNode;
  Directory_t *dirAlloc;
  Searchpath_t **checkPath;
  char ospath[MAX_OS_PATH_SHORT];
  char gameSubdir[MAX_QPATH];

  if ( localized )
    Com_sprintf( gameSubdir, MAX_QPATH, "%s/%s", gamedir, "english" );
  else
    I_strncpyz( gameSubdir, gamedir, MAX_QPATH );

  /* check if this directory is already in the search path */
  for ( sp = (Searchpath_t *)fs_searchpaths; sp; sp = sp->next )
  {
    if ( sp->dir && !Q_stricmp( sp->dir->path, basepath ) && !Q_stricmp( sp->dir->gamedir, gameSubdir ) )
    {
      if ( sp->localized != localized )
        Sys_Printf( "WARNING: game folder %s/%s added as both localized & non-localized. Using folder as %s\n",
          basepath, gameSubdir, sp->localized ? "localized" : "non-localized" );
      if ( sp->localized && sp->language != language )
        Sys_Printf( "WARNING: game golder %s/%s re-added as localized folder with different language\n", basepath, gameSubdir );
      return;
    }
  }

  /* for localized dirs, verify the directory has contents */
  if ( localized )
  {
    FS_BuildOSPathFull( basepath, gameSubdir, "", ospath, 0 );
    ospath[strlen( ospath ) - 1] = 0;
    if ( !Sys_DirectoryHasContents( ospath ) )
      return;
  }
  else
  {
    I_strncpyz( g_fsGameDir, gameSubdir, MAX_OS_PATH_SHORT );
  }

  /* allocate search path node with directory */
  searchNode = Z_Malloc( sizeof(Searchpath_t) );
  dirAlloc = Z_Malloc( sizeof(Directory_t) );
  searchNode->dir = dirAlloc;
  I_strncpyz( dirAlloc->path, basepath, MAX_OS_PATH_SHORT );
  I_strncpyz( dirAlloc->gamedir, gameSubdir, MAX_OS_PATH_SHORT );

  Assert( localized || !language, s_assertDisable_FS_AddGameDirectory );
  searchNode->localized = localized;
  searchNode->ignore = 0;
  searchNode->ignorePureCheck = !Q_stricmp(gamedir, "players");
  searchNode->language = language;

  /* insert: localized paths go after non-localized */
  checkPath = &fs_searchpaths;
  if ( localized && fs_searchpaths )
  {
    Searchpath_t *cur;
    for ( cur = *checkPath; cur; cur = cur->next )
    {
      if ( cur->localized )
        break;
      checkPath = &cur->next;
    }
  }
  searchNode->next = *checkPath;
  *checkPath = searchNode;

  FS_AddIwdFilesForGameDirectory( basepath, gameSubdir );
}

/*
================
FS_AddGameDirectoryBoth

Adds both localized and non-localized game directory.
================
*/
void FS_AddGameDirectoryBoth(const char *basepath, const char *gamedir)
{
  FS_AddGameDirectory( basepath, 1, gamedir, 0 );  /* localized first */
  FS_AddGameDirectory( basepath, 0, gamedir, 0 );  /* then non-localized */
}

/*
================
FS_StartupFull

Initializes filesystem by adding all game directories in priority order.
================
*/
int FS_StartupFull( const char *gamedir )
{
  const char *base, *home, *cd;

  Sys_Printf( "----- FS_Startup -----\n" );
  FS_RegisterDvars();

  base = fs_basepath->current.string;
  home = fs_homepath->current.string;
  cd = fs_cdpath->current.string;

  /* CoD4 0x46B0D0: base root gets the development/raw search paths plus players. */
  if ( *base )
  {
    FS_AddGameDirectoryBoth( base, "devraw_shared" );
    FS_AddGameDirectoryBoth( base, "devraw" );
    FS_AddGameDirectoryBoth( base, "raw_shared" );
    FS_AddGameDirectoryBoth( base, "raw" );
    FS_AddGameDirectoryBoth( base, "players" );
  }
  if ( *home && Q_stricmp( home, base ) )
  {
    FS_AddGameDirectoryBoth( home, "devraw_shared" );
    FS_AddGameDirectoryBoth( home, "devraw" );
    FS_AddGameDirectoryBoth( home, "raw_shared" );
    FS_AddGameDirectoryBoth( home, "raw" );
  }
  if ( *cd && Q_stricmp( cd, base ) )
  {
    FS_AddGameDirectoryBoth( cd, "devraw_shared" );
    FS_AddGameDirectoryBoth( cd, "devraw" );
    FS_AddGameDirectoryBoth( cd, "raw_shared" );
    FS_AddGameDirectoryBoth( cd, "raw" );
    FS_AddGameDirectoryBoth( cd, gamedir );
  }

  /* CoD4 adds the shared game directory before its ordinary game directory. */
  if ( *base )
  {
    char *sharedGameDir = va("%s_shared", gamedir);
    FS_AddGameDirectoryBoth( base, sharedGameDir );
    FS_AddGameDirectoryBoth( base, gamedir );
  }
  if ( *base && Q_stricmp( home, base ) )
  {
    char *sharedGameDir = va("%s_shared", gamedir);
    FS_AddGameDirectoryBoth( home, sharedGameDir );
    FS_AddGameDirectoryBoth( home, gamedir );
  }

  /* fs_basegame override */
  if ( *fs_basegame->current.string && !Q_stricmp( gamedir, "main" )
    && Q_stricmp( fs_basegame->current.string, gamedir ) )
  {
    if ( *cd )   FS_AddGameDirectoryBoth( cd, fs_basegame->current.string );
    if ( *base ) FS_AddGameDirectoryBoth( base, fs_basegame->current.string );
    if ( *home && Q_stricmp( home, base ) )
      FS_AddGameDirectoryBoth( home, fs_basegame->current.string );
  }

  /* fs_game override */
  if ( *(*fs_gameDirVar)->current.string && !Q_stricmp( gamedir, "main" )
    && Q_stricmp( (*fs_gameDirVar)->current.string, gamedir ) )
  {
    if ( *cd )   FS_AddGameDirectoryBoth( cd, (*fs_gameDirVar)->current.string );
    if ( *base ) FS_AddGameDirectoryBoth( base, (*fs_gameDirVar)->current.string );
    if ( *home && Q_stricmp( home, base ) )
      FS_AddGameDirectoryBoth( home, (*fs_gameDirVar)->current.string );
  }

  FS_DisplayPath( 0 );
  Dvar_ClearModified( *fs_gameDirVar );
  Sys_Printf( "----------------------\n" );
  return Sys_Printf( "%d files in iwd files\n", g_totalIwdFiles );
}

/*
================
FS_FindDefaultConfig

Checks for default_localize.cfg then default.cfg.
================
*/
const char *FS_FindDefaultConfig( void )
{
  int fileSize, fileHandle;

  if ( !fs_searchpaths )
    Com_ErrorLevel( 0, "Filesystem call made without initialization\n" );
  g_fsInitialized = 1;

  /* try localized config first */
  fileSize = FS_FOpenFileRead( "default_localize.cfg", &fileHandle, FS_THREAD_MAIN );
  if ( fileHandle )
  {
    FS_FCloseFile( fileHandle );
    if ( fileSize > 0 )
      return "default_localize.cfg";
  }

  /* fall back to default config */
  fileSize = FS_FOpenFileRead( "default.cfg", &fileHandle, FS_THREAD_MAIN );
  if ( fileHandle )
  {
    FS_FCloseFile( fileHandle );
    if ( fileSize > 0 )
      return "default.cfg";
  }

  return NULL;
}

/*
================
FS_InitFilesystem

Shuts down and reinitializes filesystem, validates default config.
================
*/
void FS_ClearIwdReferences(void)
{
  Searchpath_t *sp;

  for ( sp = (Searchpath_t *)fs_searchpaths; sp; sp = sp->next )
  {
    if ( sp->iwd )
      sp->iwd->referenced = 0;
  }
}

char *FS_InitFilesystem( char dummy )
{
  int fileSize;

  FS_Shutdown(0);
  FS_ClearIwdReferences();

  FS_StartupFull( "main" );

  fileSize = FS_LoadFile("default.cfg", NULL);
  if ( fileSize <= 0 )
  {
    if ( g_savedFsBasepath[0] )
    {
      Dvar_SetString( (Dvar_t *)fs_basepath, g_savedFsBasepath );
      Dvar_SetString( *fs_gameDirVar, g_savedFsGame );
      g_savedFsBasepath[0] = 0;
      g_savedFsGame[0] = 0;
      FS_InitFilesystem( dummy );
      Com_ErrorLevel( 1, "Invalid game folder\n" );
    }
    Com_ErrorLevel( 0, "Couldn't load %s.  Make sure Call of Duty is run from the correct folder.",
      "default.cfg" );
  }

  I_strncpyz( g_savedFsBasepath, fs_basepath->current.string, MAX_OS_PATH_SHORT );
  return I_strncpyz( g_savedFsGame, (*fs_gameDirVar)->current.string, MAX_OS_PATH_SHORT );
}

/*
================
FS_InitFilesystemFromArgs

Standalone filesystem init from basepath/gameName/basegame args.
================
*/
char *FS_InitFilesystemFromArgs(char *basepath, char *gameName, char *basegame)
{
  int fileSize;

  Swap_InitByteSwap();
  Dvar_Init();

  if (basepath) Dvar_SetStringByName("fs_basepath", basepath);
  if (basegame) Dvar_SetStringByName("fs_basegame", basegame);
  if (gameName) Dvar_SetStringByName("fs_game", gameName);

  FS_StartupFull("main");
  fileSize = FS_LoadFile("fileSysCheck.cfg", NULL);
  if ( fileSize <= 0 )
    Com_ErrorLevel(0, "Couldn't load %s.  Make sure Call of Duty is run from the correct folder.", "fileSysCheck.cfg");
  I_strncpyz(g_savedFsBasepath, fs_basepath->current.string, MAX_OS_PATH_SHORT);
  I_strncpyz(g_savedFsGame, (*fs_gameDirVar)->current.string, MAX_OS_PATH_SHORT);

  return NULL;
}

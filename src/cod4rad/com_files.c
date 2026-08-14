/*
 * com_files.c — Virtual file system with IWD (zip) support.
 */

#include "cod2rad64.h"

/* minizip — need real unz_file_info struct to avoid stack corruption */
#include "zlib/zlib.h"
#include "minizip/unzip.h"

static char s_assertDisable_FS_HandleForFile;
static char s_assertDisable_FS_FileForHandle;
static char s_assertDisable_FS_FileForHandle_zip;
static char s_assertDisable_FS_FileForHandle_file;
static char s_assertDisable_FS_BuildOSPath;
static char s_assertDisable_FS_BuildOSPath_qpath;
static char s_assertDisable_FS_BuildOSPath_ospath;
static char s_assertDisable_FS_filelength;
static char s_assertDisable_FS_FreeFile;
static char s_assertDisable_FS_SanitizeFilename;
static char s_assertDisable_FS_SanitizeFilename_out;
static char s_assertDisable_FS_SanitizeFilename_size;
static char s_assertDisable_FS_SanitizeFilename_overflow;
static char s_assertDisable_FS_FOpenFileRead;

typedef struct fileHandleData_s {
    FILE          *handleFile;  /* [0]   FILE* for on-disk, unzFile* for iwd */
    int            uniqueFILE;  /* [8]   if set, FS_FCloseFile calls unzClose on the archive */
    int            handleSync;  /* [12]  sync writes flag */
    int            fileSize;    /* [16]  file size */
    unsigned char  _pad[4];     /* [20]  alignment */
    intptr_t       zipFilePos;  /* [24]  position in zip file */
    void          *zipFile;     /* [32]  non-NULL = entry lives inside an iwd */
    int            streamed;    /* [40]  streamed flag */
    char           name[260];   /* [44]  filename */
} fileHandleData_t;

static fileHandleData_t fsh[MAX_FILE_HANDLES];

char g_fsBasepathCopy[MAX_OS_PATH_SHORT];
char g_fsGameDirCopy[MAX_OS_PATH_SHORT];

/*
================
FS_HashFileName

Hash a filename for lookup table. Stops at '.' (extension boundary),
normalizes backslashes to forward slashes, uses incrementing multiplier.
================
*/
int FS_HashFileName(const char *fname, int hashSize)
{
    int hash = 0;
    int mul = 119;
    int c;

    while (*fname)
    {
        c = tolower((int)(signed char)*fname);  /* binary uses movsx (sign-extend) */
        if (c == '.')
            break;
        if (c == '\\')
            c = '/';
        hash += mul * c;
        mul++;
        fname++;
    }

    {
        int temp = (hash >> 10) ^ hash;
        hash = (temp >> 10) ^ hash;  /* second XOR uses ORIGINAL hash, not temp */
    }
    return hash & (hashSize - 1);
}

/*
================
FS_HandleForFile

Finds a free file handle. Handles 1-50 for normal, 51-63 for streaming.
================
*/
int FS_HandleForFile(int streamThread)
{
    int start, count;
    int i;

    if (streamThread)
    {
        start = 51;
        count = 13;
    }
    else
    {
        start = 1;
        count = 50;
    }

    for (i = 0; i < count; i++)
    {
        if (!fsh[start + i].handleFile)
            return start + i;
    }

    if (streamThread)
    {
        Assert(0, s_assertDisable_FS_HandleForFile);
    }

    for (i = 1; i < MAX_FILE_HANDLES; i++)
        Com_Printf("FILE %2i: '%s'\n", i, fsh[i].name);

    Com_Error(1, "FS_HandleForFile: none free");
    return -1;
}

/*
================
FS_FileForHandle

Returns the FILE* for a handle index. Asserts valid range and no zip.
================
*/
FILE *FS_FileForHandle(int f)
{
    Assert(f > 0 && f < MAX_FILE_HANDLES, s_assertDisable_FS_FileForHandle);
    Assert(!fsh[f].zipFile, s_assertDisable_FS_FileForHandle_zip);
    Assert(fsh[f].handleFile, s_assertDisable_FS_FileForHandle_file);

    return fsh[f].handleFile;
}

static void *fs_searchpaths;
static char fs_gamedir[256]; /* unk_63EDC50 — current game directory */


typedef struct fileInIwd_s {
    unsigned __int64 pos;           /* +0x00: position in zip */
    char *name;                     /* +0x08: filename string */
    struct fileInIwd_s *next;       /* +0x10: hash chain next */
} fileInIwd_t; /* 24 bytes */

typedef struct {
    char iwdFilename[256];          /* +0x000 */
    char iwdBasename[256];          /* +0x100 */
    char iwdGamename[256];          /* +0x200 */
    void *handle;                   /* +0x300: unzFile */
    int numFiles;                   /* +0x308 */
    int referenced;                 /* +0x30C */
    int hashSize;                   /* +0x310 */
    int _pad314;                    /* +0x314 */
    fileInIwd_t **hashTable;        /* +0x318 */
    void *buildBuffer;              /* +0x320 */
    /* total base: 0x328 = 808 bytes, hash table appended after */
} iwd_t;

typedef struct {
    char path[256];
    char gamedir[256];
} directory_t;

static int g_totalIwdFiles;
static int g_fsInitialized;

/*
================
FS_BuildOSPath

Builds OS filesystem path from base/gamedir/qpath into ospath buffer.
Normalizes to backslashes, collapses consecutive slashes.
================
*/
char *FS_BuildOSPath(const char *base, const char *gamedir, const char *qpath, char *ospath, int lenCheck)
{
    int baseLen, gameLen, qpathLen;
    char *dst;
    char lastSlash;

    Assert(base, s_assertDisable_FS_BuildOSPath);
    Assert(qpath, s_assertDisable_FS_BuildOSPath_qpath);
    Assert(ospath, s_assertDisable_FS_BuildOSPath_ospath);

    if (!gamedir || !*gamedir)
        gamedir = fs_gamedir;

    baseLen = (int)strlen(base);
    gameLen = (int)strlen(gamedir);
    qpathLen = (int)strlen(qpath);

    if (baseLen + gameLen + qpathLen + 2 >= MAX_OSPATH)
    {
        if (!lenCheck)
            Com_Error(0, "FS_BuildOSPath: os path length exceeded\n");
        *ospath = 0;
        return ospath;
    }

    memmove(ospath, base, baseLen);
    ospath[baseLen] = '/';
    memmove(ospath + baseLen + 1, gamedir, gameLen);
    ospath[baseLen + 1 + gameLen] = '/';
    memmove(ospath + baseLen + 2 + gameLen, qpath, qpathLen + 1);

    /* normalize slashes to backslash and collapse consecutive */
    dst = ospath;
    lastSlash = 0;
    while (*ospath)
    {
        if (*ospath == '/' || *ospath == '\\')
        {
            if (!lastSlash)
            {
                *dst++ = '\\';
                lastSlash = 1;
            }
        }
        else
        {
            lastSlash = 0;
            *dst++ = *ospath;
        }
        ospath++;
    }
    *dst = 0;

    /* NOTE: returns pointer past end of input string, not the output buffer.
       No caller uses the return value. Binary behaves the same way — rax is
       not explicitly set before retn. Possible IW bug or intentionally void. */
    return ospath;
}

/*
================
FS_CopyFile

Copies a file from source path to destination path.
Creates directories along destination path as needed.
================
*/
void FS_CopyFile(const char *fromOSPath, const char *toOSPath)
{
    FILE *f;
    int len;
    void *buf;
    char *p;

    f = fopen(fromOSPath, "rb");
    if (!f)
        return;

    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);

    buf = malloc(len);
    if (fread(buf, 1, len, f) != (size_t)len)
        Com_Error(0, "Short read in FS_CopyFile()\n");
    fclose(f);

    if (strstr(toOSPath, "..") || strstr(toOSPath, "::"))
    {
        Com_Printf("WARNING: refusing to create relative path \"%s\"\n", toOSPath);
        free(buf);
        return;
    }

    /* create directories along path */
    for (p = (char *)toOSPath + 1; *p; p++)
    {
        if (*p == '\\')
        {
            *p = 0;
            Sys_Mkdir(toOSPath);
            *p = '\\';
        }
    }

    f = fopen(toOSPath, "wb");
    if (f)
    {
        if (fwrite(buf, 1, len, f) != (size_t)len)
            Com_Error(0, "Short write in FS_CopyFile()\n");
        fclose(f);
    }

    free(buf);
}

/*
================
FS_ZipFileLength

Returns uncompressed size of current file in a zip handle.
Uses unzGetCurrentFileInfo API (same as cod2map).
================
*/
static int FS_ZipFileLength(void *zipHandle)
{
    unz_file_info fi;
    unzGetCurrentFileInfo(zipHandle, &fi, NULL, 0, NULL, 0, NULL, 0);
    return (int)fi.uncompressed_size;
}

/*
================
FS_filelength

Returns size of an open file. For zip entries reads from zip struct.
For regular files uses ftell/fseek.
================
*/
int FS_filelength(int f)
{
    FILE *file;
    int pos, end;

    Assert(f, s_assertDisable_FS_filelength);

    if (!fs_searchpaths)
        Com_Error(0, "Filesystem call made without initialization");

    if (fsh[f].zipFile)
        return *(int *)((char *)fsh[f].handleFile + 0x48); /* cached unz_s.cur_file_info.uncompressed_size */

    file = FS_FileForHandle(f);
    pos = ftell(file);
    fseek(file, 0, SEEK_END);
    end = ftell(file);
    fseek(file, pos, SEEK_SET);
    return end;
}

/*
================
FS_FCloseFile

Close a file handle. For zip files closes the zip entry/archive.
Clears the handle struct after closing.
================
*/
void FS_FCloseFile(int f)
{
    if (!fs_searchpaths)
        Com_Error(0, "Filesystem call made without initialization");

    if (fsh[f].zipFile)
    {
        unzCloseCurrentFile(fsh[f].handleFile);
        if (fsh[f].uniqueFILE)
            unzClose(fsh[f].handleFile);
    }
    else
    {
        if (f)
            fclose(FS_FileForHandle(f));
    }

    memset(&fsh[f], 0, sizeof(fileHandleData_t));
}

static int fs_loadCount;

/*
================
FS_FreeFile

Frees a buffer allocated by FS_ReadFile.
================
*/
void FS_FreeFile(void *buffer)
{
    if (!fs_searchpaths)
        Com_Error(0, "Filesystem call made without initialization");

    Assert(buffer, s_assertDisable_FS_FreeFile);

    fs_loadCount--;
    Hunk_FreeTempMemory(buffer);
}

static int fs_loadStack;

typedef struct searchpath_s {
    struct searchpath_s *next;  /* +0x00 */
    iwd_t *iwd;                /* +0x08: iwd pack or NULL */
    directory_t *dir;           /* +0x10: directory or NULL */
    int localized;              /* +0x18 */
    int language;               /* +0x1C */
} searchpath_t;

static dvar_t *fs_ignoreLocalized;

/*
================
FS_DisplayPath

Prints the current search path and open file handles.
================
*/
void FS_DisplayPath(void)
{
    searchpath_t *sp;
    int i;

    if (fs_ignoreLocalized && fs_ignoreLocalized->current.enabled)
        Com_Printf("    localized assets are being ignored\n");

    Com_Printf("Current search path:\n");

    for (sp = (searchpath_t *)fs_searchpaths; sp; sp = sp->next)
    {
        if (sp->iwd)
            Com_Printf("%s (%i files)\n", sp->iwd->iwdFilename, sp->iwd->numFiles);
        else if (sp->dir)
            Com_Printf("%s/%s\n", sp->dir->path, sp->dir->gamedir);
    }

    Com_Printf("\nFile Handles:\n");
    for (i = 1; i < MAX_FILE_HANDLES; i++)
    {
        if (fsh[i].handleFile)
            Com_Printf("handle %i: %s\n", i, fsh[i].name);
    }
}

/*
================
FS_FilenameCompare

Case-insensitive path comparison, treating \ and : as /.
Returns 0 on match, -1 on mismatch.
================
*/
int FS_FilenameCompare(const char *s1, const char *s2)
{
    int c1, c2;

    do
    {
        c1 = *s1++;
        c2 = *s2++;
        if (c1 >= 'a' && c1 <= 'z')
            c1 -= 32;
        if (c2 >= 'a' && c2 <= 'z')
            c2 -= 32;
        if (c1 == '\\' || c1 == ':')
            c1 = '/';
        if (c2 == '\\' || c2 == ':')
            c2 = '/';
        if (c1 != c2)
            return -1;
    } while (c1);

    return 0;
}

static dvar_t *fs_debug;
static dvar_t *fs_copyfiles;
static dvar_t *fs_cdpath;
static dvar_t *fs_basepath;
static dvar_t *fs_basegame;
static dvar_t *fs_useOldAssets;
static dvar_t *fs_gameDirVar;
static dvar_t *fs_homepath;

/*
================
FS_RegisterDvars

Registers filesystem dvars.
================
*/
void FS_RegisterDvars(void)
{
    const char *cdPath;
    const char *basePath;
    const char *homePath;

    fs_debug = Dvar_RegisterInt("fs_debug", 0, 0, 2, DVAR_CHANGEABLE_RESET);
    fs_copyfiles = Dvar_RegisterBool("fs_copyfiles", 0, DVAR_INIT | DVAR_CHANGEABLE_RESET);
    cdPath = Sys_DefaultCDPath();
    fs_cdpath = Dvar_Register_internal("fs_cdpath", cdPath, DVAR_INIT | DVAR_CHANGEABLE_RESET);
    basePath = Sys_Cwd();
    fs_basepath = Dvar_Register_internal("fs_basepath", basePath, DVAR_INIT | DVAR_CHANGEABLE_RESET);
    fs_basegame = Dvar_Register_internal("fs_basegame", "", DVAR_INIT | DVAR_CHANGEABLE_RESET);
    fs_useOldAssets = Dvar_RegisterBool("fs_useOldAssets", 0, DVAR_CHANGEABLE_RESET);
    fs_gameDirVar = Dvar_Register_internal("fs_game", "", DVAR_SERVERINFO | DVAR_SYSTEMINFO | DVAR_INIT | DVAR_CHANGEABLE_RESET);
    fs_ignoreLocalized = Dvar_RegisterBool("fs_ignoreLocalized", 0, DVAR_LATCH | DVAR_CHEAT | DVAR_CHANGEABLE_RESET);
    homePath = Sys_DefaultHomePath();
    if (!homePath || !*homePath)
        homePath = fs_basepath->current.string;
    fs_homepath = Dvar_Register_internal("fs_homepath", homePath, DVAR_INIT | DVAR_CHANGEABLE_RESET);
}

/*
================
FS_ReadFile

Reads an entire file into a newly allocated buffer.
Returns file size, or -1 on failure.
================
*/
int FS_ReadFile(const char *qpath, void **buffer)
{
    int fileHandle;
    int len;
    void *buf;

    if (!fs_searchpaths)
        Com_Error(0, "Filesystem call made without initialization");

    if (!qpath || !*qpath)
        Com_Error(0, "FS_ReadFile with empty name\n");

    fs_loadStack = 1;
    len = FS_FOpenFileRead(qpath, &fileHandle, 0, 0);

    if (!fileHandle)
    {
        if (buffer)
            *buffer = NULL;
        return -1;
    }

    if (buffer)
    {
        fs_loadCount++;
        buf = Z_Malloc(len + 1);
        *buffer = buf;
        FS_Read(buf, len, fileHandle);
        ((char *)buf)[len] = 0;
    }

    FS_FCloseFile(fileHandle);
    return len;
}

/*
================
FS_Read

Read from an open file handle. For zip files reads via minizip.
For regular files reads via fread with retry on zero-read.
================
*/
int FS_Read(void *buffer, int len, int f)
{
    FILE *file;
    int remaining;
    int read;
    int tried;

    if (!fs_searchpaths)
        Com_Error(0, "Filesystem call made without initialization");

    if (!f)
        return 0;

    if (fsh[f].zipFile)
        return unzReadCurrentFile(fsh[f].handleFile, buffer, len);

    file = FS_FileForHandle(f);
    remaining = len;
    tried = 0;

    while (remaining)
    {
        read = fread(buffer, 1, remaining, file);
        if (read == 0)
        {
            if (tried)
                return len - remaining;
            tried = 1;
        }
        else if (read == -1)
        {
            if (f >= 51 && f < 64)
                return -1;
            Com_Error(0, "FS_Read: -1 bytes read");
        }
        buffer = (char *)buffer + read;
        remaining -= read;
    }

    return len;
}

/*
================
FS_SanitizeFilename

Sanitizes a filename: rejects ".." and "::", normalizes slashes,
collapses consecutive separators, strips leading separators.
Returns 1 on success, 0 on rejection.
================
*/
static char s_assertDisable_FS_SanitizeFilename_dstindex;

int FS_SanitizeFilename(const char *filename, char *sanitizedName, int sanitizedNameSize)
{
    int src, dst;
    char c, peek;

    Assert(filename, s_assertDisable_FS_SanitizeFilename);
    Assert(sanitizedName, s_assertDisable_FS_SanitizeFilename_out);
    Assert(sanitizedNameSize > 0, s_assertDisable_FS_SanitizeFilename_size);

    for (src = 0; filename[src] == '/' || filename[src] == '\\'; src++)
        ;

    dst = 0;
    while (filename[src])
    {
        if (filename[src] == '.' && filename[src + 1] == '.')
            return 0;
        if (filename[src] == ':' && filename[src + 1] == ':')
            return 0;

        peek = filename[src + 1];
        if (filename[src] == '.' && (peek == 0 || peek == '/' || peek == '\\'))
        {
            src++;
            continue;
        }

        Assert(dst + 1 < sanitizedNameSize, s_assertDisable_FS_SanitizeFilename_overflow);

        c = filename[src];
        if (c == '/' || c == '\\')
        {
            sanitizedName[dst++] = '/';
            while (filename[src + 1] == '/' || filename[src + 1] == '\\')
                src++;
        }
        else
        {
            sanitizedName[dst++] = c;
        }
        src++;
    }

    Assert(dst <= src, s_assertDisable_FS_SanitizeFilename_dstindex);
    sanitizedName[dst] = 0;
    return 1;
}

static char g_langBuffers[2][64];
static int g_langBufferIndex;

/*
================
FS_ExtractLanguageFromPath

Extracts language from IWD filename starting at offset 10.
Uses rotating double buffer. Stops at non-alpha character.
================
*/
const char *FS_ExtractLanguageFromPath(const char *iwdName)
{
    int idx;
    int len;
    int i;
    char *buf;

    idx = g_langBufferIndex ^ 1;
    len = (int)strlen(iwdName);
    g_langBufferIndex = idx;

    if (len < 10)
    {
        g_langBuffers[idx][0] = 0;
        return g_langBuffers[idx];
    }

    buf = g_langBuffers[idx];
    memset(buf, 0, 64);

    for (i = 10; i < 64; i++)
    {
        char c = iwdName[i];
        if (!c)
            break;
        if (!isalpha((unsigned char)c))
            break;
        buf[i - 10] = c;
    }

    return buf;
}

#define LOCALIZED_PREFIX_LEN 10
static char g_localizedBlankPrefix[LOCALIZED_PREFIX_LEN + 1] = "          ";

/*
================
FS_CompareLocalizedPaths

Sort comparator for IWD filenames. Prioritizes English localized
packs first, then falls back to case-insensitive compare.
================
*/
int FS_CompareLocalizedPaths(const char **a, const char **b)
{
    const char *sa = *a;
    const char *sb = *b;
    const char *langA, *langB;
    int ca, cb;

    if (!I_strnicmp(sa, g_localizedBlankPrefix, LOCALIZED_PREFIX_LEN) && !I_strnicmp(sb, g_localizedBlankPrefix, LOCALIZED_PREFIX_LEN))
    {
        langA = FS_ExtractLanguageFromPath(sa);
        langB = FS_ExtractLanguageFromPath(sb);

        if (!I_stricmp(langA, "english"))
        {
            if (I_stricmp(langB, "english"))
                return -1;
        }
        else if (!I_stricmp(langB, "english"))
        {
            return 1;
        }
    }

    for (;;)
    {
        ca = (signed char)*sa++;
        cb = (signed char)*sb++;
        if (isupper(ca))
            ca -= 0x20;
        if (isupper(cb))
            cb -= 0x20;
        if (ca == '\\' || ca == ':')
            ca = '/';
        if (cb == '\\' || cb == ':')
            cb = '/';
        if (ca < cb)
            return -1;
        if (ca > cb)
            return 1;
        if (ca == 0)
            return 0;
    }
}

/*
================
FS_LoadIwdFile

Opens a zip/iwd file and builds the file index.
================
*/
iwd_t *FS_LoadIwdFile(const char *zipPath, const char *basename)
{
    void *uf;
    iwd_t *pak;
    fileInIwd_t *fileArray, *entry;
    int *checksumArray;
    char *namePool;
    unsigned int numFiles, hashSize;
    int i, hash, totalNameLen;
    char nameBuf[MAX_OSPATH];
    int *checksumPtr;

    uf = unzOpen(zipPath);
    /* no null check — binary relies on unzGetGlobalInfo failing with NULL handle */

    {
        int globalInfo[2];
        if (unzGetGlobalInfo(uf, globalInfo))
            return NULL;
        numFiles = (unsigned int)globalInfo[0];
    }

    g_totalIwdFiles += numFiles;

    /* first pass: count total name string length */
    unzGoToFirstFile(uf);
    totalNameLen = 0;
    for (i = 0; i < (int)numFiles; i++)
    {
        if (unzGetCurrentFileInfo(uf, NULL, nameBuf, MAX_OSPATH, NULL, 0, NULL, 0))
            break;
        totalNameLen += (int)strlen(nameBuf) + 1;
        unzGoToNextFile(uf);
    }

    /* allocate file entries + name pool (exact size) */
    fileArray = (fileInIwd_t *)Z_Malloc(sizeof(fileInIwd_t) * numFiles + totalNameLen);
    namePool = (char *)&fileArray[numFiles];

    /* allocate checksum array */
    checksumArray = (int *)Z_Malloc(sizeof(int) * numFiles);

    /* compute hash table size: next power of 2 >= numFiles, max 1024 */
    for (hashSize = 1; hashSize <= numFiles && hashSize <= 1024; hashSize *= 2)
        ;

    /* allocate iwd_t + inline hash table */
    pak = (iwd_t *)Z_Malloc(sizeof(iwd_t) + sizeof(fileInIwd_t *) * hashSize);
    pak->hashSize = hashSize;
    pak->hashTable = (fileInIwd_t **)((char *)pak + sizeof(iwd_t));

    /* clear hash table */
    for (i = 0; i < (int)hashSize; i++)
        pak->hashTable[i] = NULL;

    I_strncpyz(pak->iwdFilename, zipPath, 256);
    I_strncpyz(pak->iwdBasename, basename, 256);

    /* strip .iwd extension */
    if (strlen(pak->iwdBasename) > 4
        && !I_stricmp(pak->iwdBasename + strlen(pak->iwdBasename) - 4, ".iwd"))
        pak->iwdBasename[strlen(pak->iwdBasename) - 4] = 0;

    pak->handle = uf;
    pak->numFiles = numFiles;

    /* second pass: read file entries, build hash table, compute checksums */
    checksumPtr = checksumArray;
    unzGoToFirstFile(uf);

    for (i = 0, entry = fileArray; i < (int)numFiles; i++, entry++)
    {
        unz_file_info fi;
        if (unzGetCurrentFileInfo(uf, &fi, nameBuf, MAX_OSPATH, NULL, 0, NULL, 0))
            break;
        if (fi.compressed_size > 0)
            *checksumPtr++ = BigLong(fi.crc);

        I_strlwr(nameBuf);
        hash = FS_HashFileName(nameBuf, pak->hashSize);
        entry->name = namePool;
        strcpy(namePool, nameBuf);
        namePool += strlen(nameBuf) + 1;

        {
            unsigned __int64 pos;
            unzGetCurrentFileInfoPosition(uf, &pos);
            entry->pos = pos;
        }

        entry->next = pak->hashTable[hash];
        pak->hashTable[hash] = entry;
        unzGoToNextFile(uf);
    }

    Z_FreeInternal(checksumArray);
    pak->buildBuffer = fileArray;
    return pak;
}

/*
================
FS_AddIwdFilesForGameDirectory

Lists and loads all .iwd files from a game directory.
================
*/
void FS_AddIwdFilesForGameDirectory(const char *basepath, const char *gamedir)
{
    char *sorted[MAX_IWD_FILES];
    char **fileList;
    int numFiles, i, localized;
    const char *curName, *lang;
    iwd_t *pakFile;
    searchpath_t *searchNode;
    searchpath_t **insertPoint;
    char ospath[MAX_OSPATH];

    FS_BuildOSPath(basepath, gamedir, "", ospath, 0);
    ospath[strlen(ospath) - 1] = 0;

    numFiles = 0;
    fileList = Sys_ListFiles(ospath, "iwd", 0, &numFiles, 0);

    if (numFiles > MAX_IWD_FILES)
    {
        Com_Printf("WARNING: Exceeded max number of iwd files in %s/%s (%i/%i)\n",
            basepath, gamedir, numFiles, MAX_IWD_FILES);
        numFiles = MAX_IWD_FILES;
    }

    for (i = 0; i < numFiles; i++)
    {
        sorted[i] = fileList[i];
        if (!I_strnicmp(sorted[i], "localized_", 10))
            memmove(sorted[i], g_localizedBlankPrefix, LOCALIZED_PREFIX_LEN);
    }

    qsort(sorted, numFiles, sizeof(char *), (int (*)(const void *, const void *))FS_CompareLocalizedPaths);

    for (i = 0; i < numFiles; i++)
    {
        curName = sorted[i];

        if (!I_strnicmp(curName, g_localizedBlankPrefix, LOCALIZED_PREFIX_LEN))
        {
            memmove((char *)curName, "localized_", 10);
            localized = 1;

            lang = FS_ExtractLanguageFromPath(curName);
            if (!*lang)
            {
                Com_Printf("WARNING: Localized assets iwd file %s/%s/%s has invalid name. Skipping.\n",
                    basepath, gamedir, curName);
                continue;
            }
            if (I_stricmp(lang, "english"))
                continue;
        }
        else
        {
            localized = 0;
        }

        FS_BuildOSPath(basepath, gamedir, curName, ospath, 0);
        pakFile = FS_LoadIwdFile(ospath, curName);
        if (!pakFile)
            continue;

        I_strncpyz(pakFile->iwdGamename, gamedir, 256);

        searchNode = (searchpath_t *)Z_Malloc(sizeof(searchpath_t));
        memset(searchNode, 0, sizeof(searchpath_t));
        searchNode->iwd = pakFile;
        searchNode->localized = localized;

        insertPoint = (searchpath_t **)&fs_searchpaths;
        if (localized && fs_searchpaths)
        {
            searchpath_t *cur;
            for (cur = (searchpath_t *)fs_searchpaths; cur; cur = cur->next)
            {
                if (cur->localized)
                    break;
                insertPoint = &cur->next;
            }
        }
        searchNode->next = *insertPoint;
        *insertPoint = searchNode;
    }

    Sys_FreeFileList(fileList);
}

/*
================
FS_AddGameDirectory

Adds a game directory to the search path.
================
*/
void FS_AddGameDirectory(const char *basepath, const char *gamedir, int localized, int language)
{
    searchpath_t *sp, *searchNode;
    directory_t *dirAlloc;
    searchpath_t **checkPath;
    char ospath[MAX_OSPATH];
    char gameSubdir[MAX_QPATH];

    if (localized)
        Com_sprintf(gameSubdir, MAX_QPATH, "%s/%s", gamedir, "english");
    else
        I_strncpyz(gameSubdir, gamedir, MAX_QPATH);

    for (sp = (searchpath_t *)fs_searchpaths; sp; sp = sp->next)
    {
        if (sp->dir && !I_stricmp(sp->dir->path, basepath) && !I_stricmp(sp->dir->gamedir, gameSubdir))
        {
            Com_Printf("WARNING: game folder %s/%s added as both localized & non-localized. Using first.\n", basepath, gameSubdir);
            if (!sp->localized)
                return;
            if (sp->language == language)
                return;
            Com_Printf("WARNING: game folder %s/%s re-added as localized with different language\n", basepath, gameSubdir);
            return;
        }
    }

    if (localized)
    {
        FS_BuildOSPath(basepath, gameSubdir, "", ospath, 0);
        ospath[strlen(ospath) - 1] = 0;
        if (!Sys_DirectoryHasContents(ospath))
            return;
    }
    else
    {
        I_strncpyz(fs_gamedir, gameSubdir, 256);
    }

    searchNode = (searchpath_t *)Z_Malloc(sizeof(searchpath_t));
    memset(searchNode, 0, sizeof(searchpath_t));
    dirAlloc = (directory_t *)Z_Malloc(sizeof(directory_t));
    searchNode->dir = dirAlloc;
    I_strncpyz(dirAlloc->path, basepath, 256);
    I_strncpyz(dirAlloc->gamedir, gameSubdir, 256);

    searchNode->localized = localized;
    searchNode->language = language;

    checkPath = (searchpath_t **)&fs_searchpaths;
    if (localized && fs_searchpaths)
    {
        searchpath_t *cur;
        for (cur = (searchpath_t *)fs_searchpaths; cur; cur = cur->next)
        {
            if (cur->localized)
                break;
            checkPath = &cur->next;
        }
    }
    searchNode->next = *checkPath;
    *checkPath = searchNode;

    FS_AddIwdFilesForGameDirectory(basepath, gameSubdir);
}

/*
================
FS_AddSearchPath

Adds both localized and non-localized game directory.
================
*/
void FS_AddSearchPath(const char *basepath, const char *gamedir)
{
    FS_AddGameDirectory(basepath, gamedir, 1, 0);
    FS_AddGameDirectory(basepath, gamedir, 0, 0);
}

/*
================
FS_Startup

Initializes filesystem by adding all game directories.
================
*/
void FS_Startup(const char *gamedir)
{
    const char *base, *home, *cd;

    Com_Printf("----- FS_Startup -----\n");
    FS_RegisterDvars();

    base = fs_basepath->current.string;
    home = fs_homepath->current.string;
    cd = fs_cdpath->current.string;

    if (fs_useOldAssets->current.enabled)
    {
        if (*base) FS_AddSearchPath(base, "tempcod");
        if (*home) FS_AddSearchPath(home, "tempcod");
    }

    if (*base)
    {
        FS_AddSearchPath(base, "devraw_shared");
        FS_AddSearchPath(base, "devraw");
        FS_AddSearchPath(base, "raw_shared");
        FS_AddSearchPath(base, "raw");
    }
    if (*home && I_stricmp(home, base))
    {
        FS_AddSearchPath(home, "devraw_shared");
        FS_AddSearchPath(home, "devraw");
        FS_AddSearchPath(home, "raw_shared");
        FS_AddSearchPath(home, "raw");
    }
    if (*cd)
    {
        FS_AddSearchPath(cd, "devraw_shared");
        FS_AddSearchPath(cd, "devraw");
        FS_AddSearchPath(cd, "raw_shared");
        FS_AddSearchPath(cd, "raw");
        FS_AddSearchPath(cd, gamedir);
    }

    if (*base) FS_AddSearchPath(base, gamedir);
    if (*base && I_stricmp(home, base))
        FS_AddSearchPath(home, gamedir);

    if (*fs_basegame->current.string && !I_stricmp(gamedir, "main")
        && I_stricmp(fs_basegame->current.string, gamedir))
    {
        if (*cd)   FS_AddSearchPath(cd, fs_basegame->current.string);
        if (*base) FS_AddSearchPath(base, fs_basegame->current.string);
        if (*home && I_stricmp(home, base))
            FS_AddSearchPath(home, fs_basegame->current.string);
    }

    if (*fs_gameDirVar->current.string && !I_stricmp(gamedir, "main")
        && I_stricmp(fs_gameDirVar->current.string, gamedir))
    {
        if (*cd)   FS_AddSearchPath(cd, fs_gameDirVar->current.string);
        if (*base) FS_AddSearchPath(base, fs_gameDirVar->current.string);
        if (*home && I_stricmp(home, base))
            FS_AddSearchPath(home, fs_gameDirVar->current.string);
    }

    FS_DisplayPath();
    Dvar_ClearModified(fs_gameDirVar);
    Com_Printf("----------------------\n");
    Com_Printf("%d files in iwd files\n", g_totalIwdFiles);
}

/*
================
LoadDefaultConfig

Finds and loads default config file.
================
*/
const char *LoadDefaultConfig(void)
{
    int fileSize;
    int fileHandle;

    FS_Startup("main");

    if (!fs_searchpaths)
        Com_Error(0, "Filesystem call made without initialization");

    /* binary has no g_fsInitialized — phantom removed */
    fs_loadStack = 1;

    fileSize = FS_FOpenFileRead("default.cfg", &fileHandle, 0, 0);
    if (fileHandle)
    {
        FS_FCloseFile(fileHandle);
        if (fileSize > 0)
            goto success;
    }

    Com_Error(0, "Couldn't load %s.  Make sure Call of Duty is run from the correct folder.", "default.cfg");

success:
    /* binary copies dvar strings to global buffers in epilogue */
    {
        I_strncpyz(g_fsBasepathCopy, fs_basepath->current.string, MAX_OS_PATH_SHORT);
        I_strncpyz(g_fsGameDirCopy, fs_gameDirVar->current.string, MAX_OS_PATH_SHORT);
    }
    return (fileSize > 0 && fileHandle) ? "default.cfg" : NULL;
}

/*
================
FS_FOpenFileRead

Opens a file for reading across search paths.
Returns file size, or -1 on failure.
================
*/
int FS_FOpenFileRead(const char *filename, int *handleOut, int uniqueFILE, int streamThread)
{
    searchpath_t *sp;
    iwd_t *pak;
    directory_t *dir;
    fileInIwd_t *entry;
    int f;
    char ospath[MAX_OSPATH];
    char sanitized[MAX_OSPATH];

    Assert(filename, s_assertDisable_FS_FOpenFileRead);

    if (!fs_searchpaths)
        Com_Error(0, "Filesystem call made without initialization");

    if (!FS_SanitizeFilename(filename, sanitized, MAX_OSPATH))
    {
        if (handleOut)
            *handleOut = 0;
        return -1;
    }

    if (!handleOut)
    {
        for (sp = (searchpath_t *)fs_searchpaths; sp; sp = sp->next)
        {
            if (sp->localized && fs_ignoreLocalized && fs_ignoreLocalized->current.enabled)
                continue;

            pak = sp->iwd;
            if (pak)
            {
                for (entry = pak->hashTable[FS_HashFileName(sanitized, pak->hashSize)]; entry; entry = entry->next)
                    if (!FS_FilenameCompare(entry->name, sanitized))
                        return 1;
                continue;
            }

            dir = sp->dir;
            if (dir)
            {
                FILE *fp;
                FS_BuildOSPath(dir->path, dir->gamedir, sanitized, ospath, streamThread);
                fp = fopen(ospath, "rb");
                if (fp) { fclose(fp); return 1; }
            }
        }
        return -1;
    }

    f = FS_HandleForFile(streamThread);
    *handleOut = f;
    fsh[f].uniqueFILE = uniqueFILE;

    for (sp = (searchpath_t *)fs_searchpaths; sp; sp = sp->next)
    {
        if (sp->localized && fs_ignoreLocalized && fs_ignoreLocalized->current.enabled)
            continue;

        pak = sp->iwd;
        if (pak)
        {
            for (entry = pak->hashTable[FS_HashFileName(sanitized, pak->hashSize)]; entry; entry = entry->next)
            {
                if (FS_FilenameCompare(entry->name, sanitized))
                    continue;

                if (!pak->referenced && !FS_IsExt(sanitized))
                    pak->referenced = 1;

                if (uniqueFILE)
                {
                    fsh[f].handleFile = (FILE *)unzReopen(pak->iwdFilename, pak->handle);
                    if (!fsh[f].handleFile)
                    {
                        if (streamThread) { FS_FCloseFile(f); *handleOut = 0; return -1; }
                        Com_Error(0, "Couldn't reopen %s", pak->iwdFilename);
                    }
                }
                else
                {
                    fsh[f].handleFile = (FILE *)pak->handle;
                }

                I_strncpyz(fsh[f].name, sanitized, sizeof(fsh[f].name));
                fsh[f].zipFile = pak;
                {
                    /* binary seeks on pak->handle, then copies 136 bytes of unz state */
                    void *saved = *(void **)fsh[f].handleFile;
                    unzSetOffset64(pak->handle, (unsigned long long)entry->pos);
                    memmove(fsh[f].handleFile, pak->handle, 136);
                    *(void **)fsh[f].handleFile = saved;
                    unzOpenCurrentFile(fsh[f].handleFile);
                    fsh[f].zipFilePos = (intptr_t)entry->pos;
                }

                if (fs_debug && fs_debug->current.integer && !streamThread)
                    Com_Printf("FS_FOpenFileRead: %s (found in '%s')\n", sanitized, pak->iwdFilename);

                return FS_ZipFileLength(fsh[f].handleFile);
            }
            continue;
        }

        dir = sp->dir;
        if (dir)
        {
            FS_BuildOSPath(dir->path, dir->gamedir, sanitized, ospath, streamThread);
            fsh[f].handleFile = fopen(ospath, "rb");
            if (fsh[f].handleFile)
            {
                I_strncpyz(fsh[f].name, sanitized, sizeof(fsh[f].name));
                fsh[f].zipFile = NULL;

                if (fs_debug && fs_debug->current.integer && !streamThread)
                    Com_Printf("FS_FOpenFileRead: %s (found in '%s/%s')\n", sanitized, dir->path, dir->gamedir);

                /* copy file from cdpath to homepath if enabled */
                if (fs_copyfiles && fs_copyfiles->current.enabled)
                {
                    if (!I_stricmp(dir->path, fs_cdpath->current.string))
                    {
                        char copyOspath[MAX_OSPATH];
                        FS_BuildOSPath(fs_homepath->current.string, dir->gamedir,
                                       sanitized, copyOspath, streamThread);
                        FS_CopyFile(ospath, copyOspath);
                    }
                }

                return FS_filelength(f);
            }
        }
    }

    if (fs_debug && fs_debug->current.integer && !streamThread)
        Com_Printf("Can't find %s\n", filename);

    if (handleOut)
        *handleOut = 0;
    return -1;
}

/*
================
FS_IsExt

Checks if a filename ends with one of several known text extensions.
Returns 1 if matched, 0 if not.
================
*/
int FS_IsExt(const char *filename)
{
    static const char *exts[] = {
        ".hlsl", ".txt", ".cfg", ".levelshots", ".menu", ".arena", ".str", ""
    };
    int fileLen;
    int extLen;
    int i;

    fileLen = (int)strlen(filename);

    for (i = 0; exts[i][0]; i++)
    {
        extLen = (int)strlen(exts[i]);
        if (I_stricmp(filename + fileLen - extLen, exts[i]) == 0)
            return 1;
    }

    return 0;
}

/*
 * cmdlib.c — File I/O, error handling, path utilities
 */

#include "cod2rad64.h"

static void (*g_errorHandler)(const char *fmt, va_list arglist);

static char s_assertDisable_SafeRead;
static char s_assertDisable_SafeWrite;
static char s_assertDisable_CreatePath;

/*
================
Error

Call error handler if set.
================
*/
void Error(const char *fmt, ...)
{
    va_list arglist;

    if (g_errorHandler)
    {
        va_start(arglist, fmt);
        g_errorHandler(fmt, arglist);
        va_end(arglist);
    }
}

/*
================
SetErrorHandler

Set the error handler function pointer.
================
*/
void SetErrorHandler(void (*handler)(const char *, va_list))
{
    g_errorHandler = handler;
}

/*
================
SafeRead

Read exactly count bytes from file, error on short read.
================
*/
int SafeRead(FILE *fp, void *buf, int count)
{
    int bytesRead;

    Assert(count > 0, s_assertDisable_SafeRead);
    bytesRead = (int)fread(buf, 1, count, fp);
    if (bytesRead != count)
        Error("File read failure - read %i of %i bytes", bytesRead, count);
    return bytesRead;
}

/*
================
SafeWrite

Write exactly count bytes to file, error on short write.
================
*/
int SafeWrite(FILE *fp, void *buf, int count)
{
    int bytesWritten;

    Assert(count >= 0, s_assertDisable_SafeWrite);
    bytesWritten = (int)fwrite(buf, 1, count, fp);
    if (bytesWritten != count)
        Error("File write failure - wrote %i of %i bytes", bytesWritten, count);
    return bytesWritten;
}

/*
================
LoadFile

Load entire file into a 4096-byte aligned buffer, returns length or -1.
================
*/
int LoadFile(const char *filename, void **bufferptr)
{
    FILE *f;
    int length;
    int allocSize;
    int pad;
    unsigned char *buf;

    *bufferptr = NULL;
    if (!filename || !strlen(filename))
        return -1;

    f = fopen(filename, "rb");
    if (!f)
        return -1;

    {
        int startPos = ftell(f);
        fseek(f, 0, SEEK_END);
        length = ftell(f);
        fseek(f, startPos, SEEK_SET);
    }

    allocSize = length + 1;
    pad = allocSize % 4096;
    if (pad > 0)
        allocSize += 4096 - pad;

    buf = (unsigned char *)malloc(allocSize);
    memset(buf, 0, allocSize);
    buf[length] = 0;
    SafeRead(f, buf, length);
    fclose(f);
    *bufferptr = buf;
    return length;
}

/*
================
StripExtension

Strip file extension in place.
================
*/
void StripExtension(const char *path)
{
    int length;

    length = (int)strlen(path) - 1;
    while (length > 0)
    {
        if (path[length] == '.')
            break;
        --length;
        if (path[length] == '/')
            return;
    }
    if (length)
        ((char *)path)[length] = 0;
}

char g_basePath[1024];
char g_gameDir[1024];
char g_qpath[1024];

/*
================
CreatePath

Find "maps" in path, extract basepath and gamedir.
================
*/
void CreatePath(const char *path)
{
    int len, i;
    int mapsLen;
    char fullPath[1024];
    char *pos;
    char *prev;
    char ch;
    int qpathLen;

    len = GetFullPathNameA(path, 1024, fullPath, NULL);
    if (len <= 0 || len >= 1024)
        Error("couldn't get full path for '%s'\n", path);

    /* normalize backslashes to forward slashes */
    for (i = 0; i < len; i++)
    {
        if (fullPath[i] == '\\')
            fullPath[i] = '/';
    }

    /* search backward for "maps" directory component */
    mapsLen = (int)strlen("maps");
    pos = &fullPath[strlen(fullPath)];

    for (;;)
    {
        if (!pos)
        {
            Error("No '%s' in '%s'\n", "maps", fullPath);
            break;
        }

        Assert(pos >= fullPath, s_assertDisable_CreatePath);
        Assert(*pos == '\0' || pos == fullPath || pos[-1] == '/', s_assertDisable_CreatePath);

        if (pos == fullPath)
        {
            pos = NULL;
            Error("No '%s' in '%s'\n", "maps", fullPath);
            break;
        }

        /* walk backward to previous path component */
        prev = pos;
        do
            --prev;
        while (prev != fullPath && prev[-1] != '/');
        pos = prev;

        if (!pos)
        {
            Error("No '%s' in '%s'\n", "maps", fullPath);
            break;
        }

        if (!_strnicmp(pos, "maps", mapsLen))
        {
            ch = pos[mapsLen];
            if (!ch || ch == '/')
                break;
        }
    }

    /* go one more directory up from "maps" to get gamedir */
    if (!pos)
    {
        pos = NULL;
        goto validate;
    }

    Assert(pos >= fullPath, s_assertDisable_CreatePath);
    Assert(*pos == '\0' || pos == fullPath || pos[-1] == '/', s_assertDisable_CreatePath);

    if (pos != fullPath)
    {
        do
            --pos;
        while (pos != fullPath && pos[-1] != '/');
    }
    else
    {
        pos = NULL;
    }

validate:
    if (!pos || pos == fullPath)
        Error("There should be two folders below '%s' in a proper install\n", "maps");

    /* extract basepath */
    len = (int)(pos - fullPath);
    memmove(g_basePath, fullPath, len);
    g_basePath[len] = 0;

    /* extract gamedir with trailing slash */
    qpathLen = 0;
    ch = *pos;
    if (ch != '/')
    {
        while (ch && ch != '/')
        {
            g_gameDir[qpathLen] = ch;
            ch = *++pos;
            qpathLen++;
        }
    }
    g_gameDir[qpathLen] = '/';
    g_gameDir[qpathLen + 1] = 0;

    Error_va("gamedir: %s\n", g_gameDir);

    if (g_qpath[0])
    {
        len = (int)strlen(g_qpath);
        if (g_qpath[len - 1] != '/')
        {
            g_qpath[len] = '/';
            g_qpath[len + 1] = 0;
        }
    }
    else
    {
        strcpy(g_qpath, g_gameDir);
    }
}

/*
================
CopyStringInternal

Allocate and copy a string.
================
*/
char *CopyStringInternal(const char *s)
{
    char *copy;
    int len;

    len = (int)strlen(s) + 1;
    copy = (char *)malloc(len);
    strcpy(copy, s);
    return copy;
}

static int g_verbosePrintEnabled;

/*
================
Error_va

Verbose printf, only prints when g_verbosePrintEnabled is set.
================
*/
void Error_va(const char *fmt, ...)
{
    va_list ap;

    if (g_verbosePrintEnabled)
    {
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
    }
}

/*
================
InitFileSystem

Initialize byte swap, dvars, and filesystem.
================
*/
void InitFileSystem(const char *basepath, const char *game, const char *basegame)
{
    Swap_Init_BigEndian();
    Dvar_Init();

    if (basepath)
        Dvar_SetStringByName("fs_basepath", basepath);

    if (basegame)
        Dvar_SetStringByName("fs_basegame", basegame);

    if (game)
        Dvar_SetStringByName("fs_game", game);

    LoadDefaultConfig();
}

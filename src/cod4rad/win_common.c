/*
 * win_common.c — Windows platform functions, directory listing, paths.
 */

#include "cod2rad64.h"

#define MAX_FOUND_FILES 4095
#define MAX_OS_PATH 256

static char sys_cwd[MAX_OS_PATH];
static char sys_cdpath[MAX_OS_PATH];

/*
================
Sys_Mkdir

Create a directory.
================
*/
int Sys_Mkdir(const char *path)
{
    return _mkdir(path);
}

/*
================
Sys_DefaultCDPath

Returns the launch directory.  The tools are normally started from the game
root while fs_basepath points one directory above it.
================
*/
const char *Sys_DefaultCDPath(void)
{
    DWORD len;

    if (sys_cdpath[0])
        return sys_cdpath;

    len = GetCurrentDirectoryA(MAX_OS_PATH, sys_cdpath);
    if (!len || len >= MAX_OS_PATH)
        sys_cdpath[0] = '\0';

    return sys_cdpath;
}

/*
================
Sys_DefaultHomePath

Returns NULL (unused, falls back to basepath).
================
*/
const char *Sys_DefaultHomePath(void)
{
    return NULL;
}

/*
================
Sys_Cwd

Returns the directory containing the executable.
================
*/
char *Sys_Cwd(void)
{
    HMODULE hModule;
    int len;
    char ch;

    if (sys_cwd[0])
        return sys_cwd;

    hModule = GetModuleHandleA(NULL);
    len = GetModuleFileNameA(hModule, sys_cwd, MAX_OS_PATH);
    if (len == MAX_OS_PATH)
        len = MAX_OS_PATH - 1;

    while (len > 0)
    {
        ch = sys_cwd[len];
        if (ch == '\\' || ch == '/' || ch == ':')
            break;
        len--;
    }
    sys_cwd[len] = 0;
    return sys_cwd;
}

/*
================
Sys_ListFiles_r

Finds files matching filter in directory, appends to list.
Does not recurse into subdirectories — skips directories entirely.
================
*/
static void Sys_ListFiles_r(const char *basedir, const char *subdirs, char *filter, char **list, int *numfiles)
{
    intptr_t findhandle;
    char search[MAX_OS_PATH];
    char filename[MAX_OS_PATH];
    char name[268];
    struct __finddata64_t finddata;

    if (*numfiles >= MAX_FOUND_FILES)
        return;

    if (strlen(subdirs))
        Com_sprintf(search, MAX_OS_PATH, "%s\\%s\\*", basedir, subdirs);
    else
        Com_sprintf(search, MAX_OS_PATH, "%s\\*", basedir);

    findhandle = _findfirst64(search, &finddata);
    if (findhandle == -1)
        return;

    do
    {
        if ((finddata.attrib & 0x10) == 0
            || (I_stricmp(finddata.name, ".") && I_stricmp(finddata.name, "..") && I_stricmp(finddata.name, "CVS")))
        {
            if (*numfiles >= MAX_FOUND_FILES)
            {
                _findclose(findhandle);
                return;
            }
            if (subdirs)
                Com_sprintf(filename, MAX_OS_PATH, "%s\\%s", subdirs, finddata.name);
            else
                Com_sprintf(filename, MAX_OS_PATH, "%s", finddata.name);

            if (Com_FilterPath(filter, filename, 0))
                list[(*numfiles)++] = CopyStringInternal(filename);
        }
    } while (_findnext64(findhandle, &finddata) != -1);

    _findclose(findhandle);
}

/*
================
Sys_ListFiles

Lists files matching extension in a directory. Returns allocated array.
================
*/
char **Sys_ListFiles(const char *directory, const char *extension, char *filter, int *numfiles, int wantsubs)
{
    intptr_t findhandle;
    char search[MAX_OS_PATH];
    char extSearch[MAX_OS_PATH];
    struct __finddata64_t finddata;
    int nfiles = 0;
    char *listbuf[MAX_FOUND_FILES];
    char **list;
    int dirflag;

    if (filter)
    {
        nfiles = 0;
        Sys_ListFiles_r(directory, "", filter, listbuf, &nfiles);
        *numfiles = nfiles;
        listbuf[nfiles] = NULL;
        if (!nfiles)
            return NULL;
        list = (char **)Z_Malloc(sizeof(char *) * (nfiles + 1));
        if (nfiles > 0)
            memmove(list, listbuf, 8 * nfiles);
        list[nfiles] = NULL;
        return list;
    }

    if (extension)
    {
        if (extension[0] == '/' && !extension[1])
        {
            extension = "";
            dirflag = 0;
        }
        else
        {
            dirflag = 0x10;
        }
    }
    else
    {
        extension = "";
        dirflag = 0x10;
    }

    if (*extension)
        Com_sprintf(search, MAX_OS_PATH, "%s\\*.%s", directory, extension);
    else
        Com_sprintf(search, MAX_OS_PATH, "%s\\*", directory);

    nfiles = 0;
    findhandle = _findfirst64(search, &finddata);
    if (findhandle == -1)
    {
        *numfiles = 0;
        return NULL;
    }

    do
    {
        if (wantsubs)
        {
            if ((finddata.attrib & 0x10) == 0)
                continue;
        }
        else
        {
            if (dirflag == (finddata.attrib & 0x10)) /* binary XORs and skips when equal */
                continue;
        }

        if ((finddata.attrib & 0x10)
            && (!I_stricmp(finddata.name, ".") || !I_stricmp(finddata.name, "..") || !I_stricmp(finddata.name, "CVS")))
            continue;

        if (*extension)
        {
            Com_sprintf(extSearch, MAX_OS_PATH, "*.%s", extension);
            if (I_stristr(extSearch, finddata.name))
                continue;
        }

        listbuf[nfiles++] = CopyStringInternal(finddata.name);
        if (nfiles == MAX_FOUND_FILES)
            break;
    } while (_findnext64(findhandle, &finddata) != -1);

    listbuf[nfiles] = NULL;
    _findclose(findhandle);
    *numfiles = nfiles;
    if (!nfiles)
        return NULL;

    list = (char **)Z_Malloc(sizeof(char *) * (nfiles + 1));
    if (nfiles > 0)
        memmove(list, listbuf, 8 * nfiles);
    list[nfiles] = NULL;
    return list;
}

/*
================
Sys_FreeFileList

Frees a file list returned by Sys_ListFiles.
================
*/
void Sys_FreeFileList(char **list)
{
    int i;

    if (list)
    {
        for (i = 0; list[i]; i++)
            free(list[i]);
        free(list);
    }
}

/*
================
Sys_DirectoryHasContents

Returns 1 if directory has files or subdirs (excluding ./../CVS).
================
*/
int Sys_DirectoryHasContents(const char *directory)
{
    intptr_t findhandle;
    char search[MAX_OS_PATH];
    struct __finddata64_t finddata;

    Com_sprintf(search, MAX_OS_PATH, "%s\\*", directory);
    findhandle = _findfirst64(search, &finddata);
    if (findhandle == -1)
        return 0;

    do
    {
        if ((finddata.attrib & _A_SUBDIR)
            && (!I_stricmp(finddata.name, ".") || !I_stricmp(finddata.name, "..") || !I_stricmp(finddata.name, "CVS")))
            continue;

        return 1;
    } while (_findnext64(findhandle, &finddata) != -1);

    return 0;
}

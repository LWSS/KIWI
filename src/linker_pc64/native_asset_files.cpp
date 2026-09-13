#include "native_asset_files.h"
#include <database64/db_package.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <windows.h>
#include <cod4rad/minizip/unzip.h>

// -1 means a read/format error, zero means absent, one means successfully read.
static int ReadArchive(const char *path, const char *relative, void **data, size_t *size, size_t limit)
{
    unzFile archive = unzOpen64(path);
    if (!archive)
    {
        return -1;
    }
    const int located = unzLocateFile(archive, relative, 2);
    if (located != UNZ_OK)
    {
        unzClose(archive);
        return located == UNZ_END_OF_LIST_OF_FILE ? 0 : -1;
    }
    unz_file_info64 info = {};
    bool valid = unzGetCurrentFileInfo64(archive, &info, NULL, 0, NULL, 0, NULL, 0) == UNZ_OK &&
                 info.uncompressed_size <= limit && info.uncompressed_size < SIZE_MAX && !(info.flag & 1);
    const bool opened = valid && unzOpenCurrentFile(archive) == UNZ_OK;
    valid = valid && opened;
    uint8_t *bytes = valid ? (uint8_t *)malloc((size_t)info.uncompressed_size + 1) : NULL;
    valid = valid && bytes;
    size_t offset = 0;
    while (valid && offset < info.uncompressed_size)
    {
        const size_t remaining = (size_t)info.uncompressed_size - offset;
        const unsigned int chunk = (unsigned int)(remaining > 1048576 ? 1048576 : remaining);
        const int read = unzReadCurrentFile(archive, bytes + offset, chunk);
        valid = read > 0 && (unsigned int)read <= chunk;
        if (valid)
        {
            offset += read;
        }
    }
    if (valid)
    {
        uint8_t extra;
        valid = unzReadCurrentFile(archive, &extra, 1) == 0;
    }
    if (opened)
    {
        valid = unzCloseCurrentFile(archive) == UNZ_OK && valid;
    }
    valid = unzClose(archive) == UNZ_OK && valid;
    if (!valid)
    {
        free(bytes);
        return -1;
    }
    bytes[offset] = 0;
    *data = bytes;
    *size = offset;
    return 1;
}

static int CompareArchiveNames(const void *left, const void *right)
{
    return _stricmp(*(const char *const *)right, *(const char *const *)left);
}

static bool VisitAssetName(const char *name, const char *directory, const char *extension,
                            LinkerRawAssetVisitor visit, void *context)
{
    char normalized[2048];
    if (!DB64_NormalizePath(name, normalized, sizeof(normalized)))
    {
        return true;
    }
    const size_t prefix = strlen(directory);
    if (strncmp(normalized, directory, prefix) || normalized[prefix] != '/' || strchr(normalized + prefix + 1, '/'))
    {
        return true;
    }
    const size_t length = strlen(normalized), suffix = strlen(extension);
    if (length < suffix || _stricmp(normalized + length - suffix, extension))
    {
        return true;
    }
    return visit(normalized, context);
}

static bool VisitArchiveAssets(const char *path, const char *directory, const char *extension,
                                LinkerRawAssetVisitor visit, void *context)
{
    unzFile archive = unzOpen64(path);
    if (!archive)
    {
        return false;
    }
    int status = unzGoToFirstFile(archive);
    bool valid = true;
    while (valid && status == UNZ_OK)
    {
        unz_file_info64 info = {};
        char name[2048];
        valid = unzGetCurrentFileInfo64(archive, &info, name, sizeof(name), NULL, 0, NULL, 0) == UNZ_OK;
        if (valid && info.size_filename < sizeof(name))
        {
            name[info.size_filename] = 0;
            valid = VisitAssetName(name, directory, extension, visit, context);
        }
        if (valid)
        {
            status = unzGoToNextFile(archive);
        }
    }
    valid = valid && status == UNZ_END_OF_LIST_OF_FILE;
    return unzClose(archive) == UNZ_OK && valid;
}

bool Linker_EnumerateRawAssetFiles(const char *root, const char *directory, const char *extension,
                                   LinkerRawAssetVisitor visit, void *context)
{
    char normalized[2048], pattern[32768];
    if (!root || !directory || !extension || !visit || (extension[0] && extension[0] != '.') || strchr(extension, '/') ||
        strchr(extension, '\\') || strchr(extension, '*') || strchr(extension, '?') ||
        !DB64_NormalizePath(directory, normalized, sizeof(normalized)))
    {
        return false;
    }
    const char *locations[] = {"raw", "main"};
    WIN32_FIND_DATAA entry;
    HANDLE search;
    bool valid = true;
    for (int i = 0; valid && i < 2; ++i)
    {
        if (snprintf(pattern, sizeof(pattern), "%s/%s/%s/*", root, locations[i], normalized) >= sizeof(pattern))
        {
            return false;
        }
        search = FindFirstFileA(pattern, &entry);
        if (search != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                {
                    continue;
                }
                char name[2048];
                valid = snprintf(name, sizeof(name), "%s/%s", normalized, entry.cFileName) < sizeof(name) &&
                        VisitAssetName(name, normalized, extension, visit, context);
            } while (valid && FindNextFileA(search, &entry));
            valid = valid && GetLastError() == ERROR_NO_MORE_FILES;
            FindClose(search);
        }
        else
        {
            const DWORD error = GetLastError();
            valid = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        }
    }
    for (int i = 0; valid && i < 2; ++i)
    {
        if (snprintf(pattern, sizeof(pattern), "%s/%s/*.iwd", root, locations[i]) >= sizeof(pattern))
        {
            return false;
        }
        search = FindFirstFileA(pattern, &entry);
        if (search == INVALID_HANDLE_VALUE)
        {
            const DWORD error = GetLastError();
            valid = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
            continue;
        }
        do
        {
            if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !_strnicmp(entry.cFileName, "localized_", 10))
            {
                continue;
            }
            char path[32768];
            valid = snprintf(path, sizeof(path), "%s/%s/%s", root, locations[i], entry.cFileName) < sizeof(path) &&
                    VisitArchiveAssets(path, normalized, extension, visit, context);
        } while (valid && FindNextFileA(search, &entry));
        valid = valid && GetLastError() == ERROR_NO_MORE_FILES;
        FindClose(search);
    }
    return valid;
}

static int ReadArchives(const char *root, const char *directory, const char *relative, void **data, size_t *size,
                        size_t limit, const char *language = NULL)
{
    char localizedPrefix[96];
    if (language && snprintf(localizedPrefix, sizeof(localizedPrefix), "localized_%s_", language) >= sizeof(localizedPrefix))
    {
        return -1;
    }
    char path[32768];
    if (snprintf(path, sizeof(path), "%s/%s/*.iwd", root, directory) >= sizeof(path))
    {
        return -1;
    }
    WIN32_FIND_DATAA entry;
    HANDLE search = FindFirstFileA(path, &entry);
    if (search == INVALID_HANDLE_VALUE)
    {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? 0 : -1;
    }
    char **names = (char **)calloc(4096, sizeof(char *));
    unsigned int count = 0;
    bool valid = names != NULL;
    if (valid)
    {
        do
        {
            const bool matches = language ?
                !_strnicmp(entry.cFileName, localizedPrefix, strlen(localizedPrefix)) :
                _strnicmp(entry.cFileName, "localized_", 10) != 0;
            if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !matches)
            {
                continue;
            }
            if (count == 4096 || !(names[count] = _strdup(entry.cFileName)))
            {
                valid = false;
                break;
            }
            ++count;
        } while (FindNextFileA(search, &entry));
        if (valid)
        {
            valid = GetLastError() == ERROR_NO_MORE_FILES;
        }
    }
    FindClose(search);
    int result = valid ? 0 : -1;
    if (valid)
    {
        qsort(names, count, sizeof(char *), CompareArchiveNames);
        for (unsigned int i = 0; !result && i < count; ++i)
        {
            if (snprintf(path, sizeof(path), "%s/%s/%s", root, directory, names[i]) >= sizeof(path))
            {
                result = -1;
                break;
            }
            result = ReadArchive(path, relative, data, size, limit);
        }
    }
    for (unsigned int i = 0; i < count; ++i)
    {
        free(names[i]);
    }
    free(names);
    return result;
}
static int ReadLoose(const char *root, const char *directory, const char *relative,
                     void **data, size_t *size, size_t limit)
{
    char path[32768];
    if (snprintf(path, sizeof(path), "%s/%s/%s", root, directory, relative) >= sizeof(path))
    {
        return -1;
    }
    FILE *file = fopen(path, "rb");
    if (!file)
    {
        return errno == ENOENT || errno == ENOTDIR ? 0 : -1;
    }
    bool valid = !_fseeki64(file, 0, SEEK_END);
    const int64_t length = valid ? _ftelli64(file) : -1;
    valid = length >= 0 && (uint64_t)length <= limit && !_fseeki64(file, 0, SEEK_SET);
    uint8_t *bytes = valid ? (uint8_t *)malloc((size_t)length + 1) : NULL;
    valid = valid && bytes && fread(bytes, 1, (size_t)length, file) == (size_t)length;
    valid = fclose(file) == 0 && valid;
    if (!valid)
    {
        free(bytes);
        return -1;
    }
    bytes[length] = 0;
    *data = bytes;
    *size = (size_t)length;
    return 1;
}

bool Linker_ReadRawAssetFile(const char *root, const char *relative, void **data, size_t *size, size_t limit,
                             bool *found)
{
    if (found)
    {
        *found = false;
    }
    if (!data || !size)
    {
        return false;
    }
    *data = NULL;
    *size = 0;
    char normalized[2048];
    if (!root || !relative || limit == SIZE_MAX ||
        !DB64_NormalizePath(relative, normalized, sizeof(normalized)))
    {
        return false;
    }
    const char *locations[] = {"raw", "main"};
    int status = 0;
    for (int i = 0; !status && i < 2; ++i)
    {
        status = ReadLoose(root, locations[i], normalized, data, size, limit);
        if (!status)
        {
            status = ReadArchives(root, locations[i], normalized, data, size, limit);
        }
    }
    if (found)
    {
        *found = status != 0;
    }
    return status == 1;
}

bool Linker_ReadLocalizedAssetFile(const char *root, const char *relative, const char *language,
                                  void **data, size_t *size, size_t limit, bool *found)
{
    if (found)
    {
        *found = false;
    }
    if (!data || !size)
    {
        return false;
    }
    *data = NULL;
    *size = 0;
    if (!root || !relative || limit == SIZE_MAX)
    {
        return false;
    }
    if (language && language[0])
    {
        char path[2048];
        if (!DB64_ValidMapName(language) || snprintf(path, sizeof(path), "%s/%s", language, relative) >= sizeof(path))
        {
            return false;
        }
        bool localizedFound = false;
        const bool ok = Linker_ReadRawAssetFile(root, path, data, size, limit, &localizedFound);
        if (localizedFound)
        {
            if (found)
            {
                *found = true;
            }
            return ok;
        }
        char normalized[2048];
        if (!DB64_NormalizePath(relative, normalized, sizeof(normalized)))
        {
            return false;
        }
        int status = ReadArchives(root, "raw", normalized, data, size, limit, language);
        if (!status)
        {
            status = ReadArchives(root, "main", normalized, data, size, limit, language);
        }
        if (status)
        {
            if (found)
            {
                *found = true;
            }
            return status == 1;
        }
    }
    return Linker_ReadRawAssetFile(root, relative, data, size, limit, found);
}

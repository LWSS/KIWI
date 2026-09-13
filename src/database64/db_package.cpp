#define NOMINMAX
#include "db_package.h"
#include <windows.h>
#include <zlib/zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdarg.h>

struct DB64Package
{
    HANDLE file;
    HANDLE mapping;
    const uint8_t *view;
    uint64_t length;
    const DB64PackageHeader *header;
    const DB64PackageEntry *entries;
    uint8_t **buffers;
};

static bool DB64_Error(char *error, size_t size, const char *format, ...)
{
    if (error && size)
    {
        va_list args;
        va_start(args, format);
        vsnprintf(error, size, format, args);
        va_end(args);
        error[size - 1] = 0;
    }
    return false;
}

bool DB64_NormalizePath(const char *path, char *out, size_t capacity)
{
    if (!path || !out || !capacity || !path[0] || path[0] == '/' || path[0] == '\\')
    {
        return false;
    }
    size_t length = 0;
    size_t component = 0;
    for (const unsigned char *p = (const unsigned char *)path;; ++p)
    {
        unsigned char c = *p;
        if (c == '\\')
        {
            c = '/';
        }
        if (c == 0 || c == '/')
        {
            const size_t n = length - component;
            if (!n || (n == 1 && out[component] == '.') ||
                (n == 2 && out[component] == '.' && out[component + 1] == '.'))
            {
                return false;
            }
            component = length + 1;
        }
        if (length + 1 >= capacity || (c && (c < 32 || c >= 127 || strchr(":*?\"<>|", c))))
        {
            return false;
        }
        if (c >= 'A' && c <= 'Z')
        {
            c += 'a' - 'A';
        }
        out[length++] = (char)c;
        if (!c)
        {
            return true;
        }
    }
}

bool DB64_ValidMapName(const char *name)
{
    if (!name || !name[0] || strlen(name) >= 64)
    {
        return false;
    }
    for (const char *p = name; *p; ++p)
    {
        if (!(*p >= 'a' && *p <= 'z') && !(*p >= '0' && *p <= '9') && *p != '_')
        {
            return false;
        }
    }
    return true;
}

static bool DB64_IsMap(const char *path)
{
    size_t length = strlen(path);
    return length >= 7 && !strcmp(path + length - 7, ".d3dbsp");
}

static bool DB64_MatchesMap(const char *path, const char *map)
{
    char expected[DB64_PACKAGE_PATH];
    snprintf(expected, sizeof(expected), "maps/mp/%s.d3dbsp", map);
    if (!strcmp(path, expected))
    {
        return true;
    }
    snprintf(expected, sizeof(expected), "maps/%s.d3dbsp", map);
    return !strcmp(path, expected);
}

static bool DB64_Range(uint64_t offset, uint64_t size, uint64_t length)
{
    return offset <= length && size <= length - offset;
}

void DB64_ClosePackage(DB64Package *package)
{
    if (!package)
    {
        return;
    }
    if (package->buffers)
    {
        for (uint64_t i = 0; i < package->header->fileCount; ++i)
        {
            free(package->buffers[i]);
        }
        free(package->buffers);
    }
    if (package->view)
    {
        UnmapViewOfFile(package->view);
    }
    if (package->mapping)
    {
        CloseHandle(package->mapping);
    }
    if (package->file && package->file != INVALID_HANDLE_VALUE)
    {
        CloseHandle(package->file);
    }
    free(package);
}

DB64Package *DB64_OpenPackage(const char *path, char *error, size_t errorSize)
{
    DB64Package *p = (DB64Package *)calloc(1, sizeof(DB64Package));
    if (!p)
    {
        DB64_Error(error, errorSize, "Out of memory opening package");
        return NULL;
    }
    p->file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER length;
    if (p->file == INVALID_HANDLE_VALUE || !GetFileSizeEx(p->file, &length) ||
        length.QuadPart < (LONGLONG)sizeof(DB64PackageHeader) || (uint64_t)length.QuadPart > SIZE_MAX)
    {
        DB64_Error(error, errorSize, "Cannot open package or truncated header: %s", path);
        DB64_ClosePackage(p);
        return NULL;
    }
    p->length = (uint64_t)length.QuadPart;
    p->mapping = CreateFileMappingA(p->file, NULL, PAGE_READONLY, 0, 0, NULL);
    if (p->mapping)
    {
        p->view = (const uint8_t *)MapViewOfFile(p->mapping, FILE_MAP_READ, 0, 0, 0);
    }
    if (!p->view)
    {
        DB64_Error(error, errorSize, "Cannot map package: %s", path);
        DB64_ClosePackage(p);
        return NULL;
    }
    p->header = (const DB64PackageHeader *)p->view;
    const DB64PackageHeader *h = p->header;
    if (memcmp(h->magic, DB64_PACKAGE_MAGIC, 8) || h->version != DB64_PACKAGE_VERSION || h->pointerSize != 8 ||
        h->flags || h->reserved || h->fileSize != p->length || !h->fileCount || h->fileCount > DB64_PACKAGE_MAX_FILES ||
        !memchr(h->mapName, 0, sizeof(h->mapName)) || !DB64_ValidMapName(h->mapName) ||
        h->directoryOffset < sizeof(DB64PackageHeader) || (h->directoryOffset & 7) ||
        !DB64_Range(h->directoryOffset, h->fileCount * sizeof(DB64PackageEntry), p->length) ||
        h->directoryOffset + h->fileCount * sizeof(DB64PackageEntry) != p->length)
    {
        DB64_Error(error, errorSize, "Invalid or incompatible KIWI package header: %s", path);
        DB64_ClosePackage(p);
        return NULL;
    }
    p->entries = (const DB64PackageEntry *)(p->view + h->directoryOffset);
    uint64_t previousEnd = sizeof(DB64PackageHeader);
    unsigned int maps = 0;
    for (uint64_t i = 0; i < h->fileCount; ++i)
    {
        const DB64PackageEntry *e = &p->entries[i];
        char normalized[DB64_PACKAGE_PATH];
        if (!memchr(e->name, 0, sizeof(e->name)) || !DB64_NormalizePath(e->name, normalized, sizeof(normalized)) ||
            strcmp(normalized, e->name) || (i && strcmp(p->entries[i - 1].name, e->name) >= 0) || e->compression > 1 ||
            e->size > INT_MAX - 1 || e->storedSize > UINT_MAX || (!e->compression && e->size != e->storedSize) ||
            (e->offset & 7) || e->offset < previousEnd || !DB64_Range(e->offset, e->storedSize, h->directoryOffset))
        {
            DB64_Error(error, errorSize, "Invalid package directory entry %llu", (unsigned long long)i);
            DB64_ClosePackage(p);
            return NULL;
        }
        previousEnd = e->offset + e->storedSize;
        if (DB64_IsMap(e->name))
        {
            ++maps;
            if (!DB64_MatchesMap(e->name, h->mapName))
            {
                maps += 2;
            }
        }
    }
    if (maps != 1)
    {
        DB64_Error(error, errorSize, "A map package must contain exactly its named BSP");
        DB64_ClosePackage(p);
        return NULL;
    }
    p->buffers = (uint8_t **)calloc((size_t)h->fileCount, sizeof(uint8_t *));
    if (!p->buffers)
    {
        DB64_Error(error, errorSize, "Out of memory allocating package cache");
        DB64_ClosePackage(p);
        return NULL;
    }
    return p;
}

const DB64PackageHeader *DB64_PackageHeader(const DB64Package *p)
{
    return p ? p->header : NULL;
}

const DB64PackageEntry *DB64_PackageEntryAt(const DB64Package *p, uint64_t index)
{
    return p && index < p->header->fileCount ? &p->entries[index] : NULL;
}

int64_t DB64_FindPackageFile(const DB64Package *p, const char *name)
{
    char normalized[DB64_PACKAGE_PATH];
    if (!p || !DB64_NormalizePath(name, normalized, sizeof(normalized)))
    {
        return -1;
    }
    uint64_t low = 0;
    uint64_t high = p->header->fileCount;
    while (low < high)
    {
        const uint64_t mid = low + (high - low) / 2;
        const int cmp = strcmp(normalized, p->entries[mid].name);
        if (!cmp)
        {
            return (int64_t)mid;
        }
        if (cmp < 0)
        {
            high = mid;
        }
        else
        {
            low = mid + 1;
        }
    }
    return -1;
}

const uint8_t *DB64_ReadPackageFile(DB64Package *p, uint64_t index, char *error, size_t errorSize)
{
    if (!p || index >= p->header->fileCount)
    {
        DB64_Error(error, errorSize, "Invalid package file index");
        return NULL;
    }
    if (p->buffers[index])
    {
        return p->buffers[index];
    }
    const DB64PackageEntry *e = &p->entries[index];
    uint8_t *buffer = (uint8_t *)malloc((size_t)e->size + 1);
    if (!buffer)
    {
        DB64_Error(error, errorSize, "Out of memory reading %s", e->name);
        return NULL;
    }
    bool ok = true;
    if (e->compression)
    {
        z_stream stream = {};
        stream.next_in = (Bytef *)(p->view + e->offset);
        stream.avail_in = (uInt)e->storedSize;
        stream.next_out = buffer;
        stream.avail_out = (uInt)e->size + 1;
        if (inflateInit(&stream) != Z_OK)
        {
            ok = false;
        }
        else
        {
            ok = inflate(&stream, Z_FINISH) == Z_STREAM_END && stream.total_out == e->size &&
                 stream.total_in == e->storedSize;
            inflateEnd(&stream);
        }
    }
    else
    {
        memcpy(buffer, p->view + e->offset, (size_t)e->size);
    }
    if (!ok || crc32(0, buffer, (uInt)e->size) != e->crc)
    {
        free(buffer);
        DB64_Error(error, errorSize, "Corrupt package data: %s", e->name);
        return NULL;
    }
    buffer[e->size] = 0;
    p->buffers[index] = buffer;
    return buffer;
}

bool DB64_VerifyPackage(DB64Package *package, char *error, size_t errorSize)
{
    for (uint64_t i = 0; i < package->header->fileCount; ++i)
    {
        const bool cached = package->buffers[i] != NULL;
        if (!DB64_ReadPackageFile(package, i, error, errorSize))
        {
            return false;
        }
        // Verification must not retain an entire raw tree in RAM. Preserve any
        // cache entries the caller already owns through ReadPackageFile.
        if (!cached)
        {
            free(package->buffers[i]);
            package->buffers[i] = NULL;
        }
    }
    return true;
}

struct DB64WriteEntry
{
    DB64PackageEntry entry;
    const char *path;
};

static int DB64_CompareEntries(const void *a, const void *b)
{
    return strcmp(((const DB64WriteEntry *)a)->entry.name, ((const DB64WriteEntry *)b)->entry.name);
}

static bool DB64_Pad(FILE *file)
{
    const uint64_t position = (uint64_t)_ftelli64(file);
    const size_t padding = (size_t)((-position) & 7);
    const uint64_t zero = 0;
    return fwrite(&zero, 1, padding, file) == padding;
}

bool DB64_WritePackage(const char *path, const char *mapName, const DB64PackageInput *inputs, size_t count,
                       bool compress, char *error, size_t errorSize)
{
    if (!DB64_ValidMapName(mapName) || !count || count > DB64_PACKAGE_MAX_FILES)
    {
        return DB64_Error(error, errorSize, "Invalid map name or package file count");
    }
    DB64WriteEntry *entries = (DB64WriteEntry *)calloc(count, sizeof(DB64WriteEntry));
    if (!entries)
    {
        return DB64_Error(error, errorSize, "Out of memory building package directory");
    }
    bool ok = true;
    unsigned int maps = 0;
    for (size_t i = 0; i < count; ++i)
    {
        entries[i].path = inputs[i].path;
        if (!DB64_NormalizePath(inputs[i].name, entries[i].entry.name, DB64_PACKAGE_PATH))
        {
            ok = DB64_Error(error, errorSize, "Invalid package path: %s", inputs[i].name);
            break;
        }
        if (DB64_IsMap(entries[i].entry.name))
        {
            maps += DB64_MatchesMap(entries[i].entry.name, mapName) ? 1 : 3;
        }
    }
    if (ok)
    {
        qsort(entries, count, sizeof(DB64WriteEntry), DB64_CompareEntries);
    }
    for (size_t i = 1; ok && i < count; ++i)
    {
        if (!strcmp(entries[i - 1].entry.name, entries[i].entry.name))
        {
            ok = DB64_Error(error, errorSize, "Duplicate package path: %s", entries[i].entry.name);
        }
    }
    if (ok && maps != 1)
    {
        ok = DB64_Error(error, errorSize, "Package must contain exactly one BSP matching %s", mapName);
    }
    char temporary[32768];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp.%lu", path, GetCurrentProcessId()) >= sizeof(temporary))
    {
        ok = DB64_Error(error, errorSize, "Output path too long");
    }
    FILE *out = NULL;
    if (ok && fopen_s(&out, temporary, "wb") != 0)
    {
        ok = DB64_Error(error, errorSize, "Cannot write %s", temporary);
    }
    DB64PackageHeader header = {};
    memcpy(header.magic, DB64_PACKAGE_MAGIC, 8);
    header.version = DB64_PACKAGE_VERSION;
    header.pointerSize = 8;
    header.fileCount = count;
    strcpy_s(header.mapName, mapName);
    if (ok && fwrite(&header, sizeof(DB64PackageHeader), 1, out) != 1)
    {
        ok = DB64_Error(error, errorSize, "Cannot write package header");
    }
    for (size_t i = 0; ok && i < count; ++i)
    {
        FILE *in = NULL;
        if (fopen_s(&in, entries[i].path, "rb") != 0)
        {
            ok = DB64_Error(error, errorSize, "Cannot read %s", entries[i].path);
            break;
        }
        _fseeki64(in, 0, SEEK_END);
        const int64_t size = _ftelli64(in);
        _fseeki64(in, 0, SEEK_SET);
        if (size < 0 || size > INT_MAX - 1)
        {
            fclose(in);
            ok = DB64_Error(error, errorSize, "Asset exceeds engine file-size limit: %s", entries[i].path);
            break;
        }
        uint8_t *bytes = (uint8_t *)malloc((size_t)size + 1);
        if (!bytes || fread(bytes, 1, (size_t)size, in) != (size_t)size)
        {
            free(bytes);
            fclose(in);
            ok = DB64_Error(error, errorSize, "Cannot read complete asset: %s", entries[i].path);
            break;
        }
        fclose(in);
        DB64PackageEntry *entry = &entries[i].entry;
        entry->size = size;
        entry->storedSize = size;
        entry->crc = crc32(0, bytes, (uInt)size);
        uint8_t *compressed = NULL;
        if (compress && size)
        {
            // Bundled zlib 1.1.4 requires source size + 0.1% + 12 bytes.
            uLongf compressedSize = (uLong)size + (uLong)size / 1000 + 64;
            compressed = (uint8_t *)malloc(compressedSize);
            if (!compressed || compress2(compressed, &compressedSize, bytes, (uLong)size, Z_BEST_SPEED) != Z_OK)
            {
                free(bytes);
                free(compressed);
                ok = DB64_Error(error, errorSize, "Compression failed: %s", entry->name);
                break;
            }
            if (compressedSize < (uint64_t)size)
            {
                entry->compression = 1;
                entry->storedSize = compressedSize;
            }
        }
        ok = DB64_Pad(out);
        entry->offset = (uint64_t)_ftelli64(out);
        const uint8_t *stored = entry->compression ? compressed : bytes;
        ok = ok && fwrite(stored, 1, (size_t)entry->storedSize, out) == entry->storedSize;
        free(bytes);
        free(compressed);
        if (!ok)
        {
            DB64_Error(error, errorSize, "Cannot write asset: %s", entry->name);
        }
    }
    if (ok)
    {
        ok = DB64_Pad(out);
        header.directoryOffset = (uint64_t)_ftelli64(out);
        for (size_t i = 0; ok && i < count; ++i)
        {
            ok = fwrite(&entries[i].entry, sizeof(DB64PackageEntry), 1, out) == 1;
        }
        header.fileSize = (uint64_t)_ftelli64(out);
        ok = ok && !_fseeki64(out, 0, SEEK_SET) && fwrite(&header, sizeof(DB64PackageHeader), 1, out) == 1;
        if (!ok)
        {
            DB64_Error(error, errorSize, "Cannot finish package directory");
        }
    }
    if (out && fclose(out) != 0)
    {
        ok = DB64_Error(error, errorSize, "Cannot flush package output");
    }
    free(entries);
    if (ok)
    {
        // Fully validate output before atomically replacing a previously good build.
        DB64Package *verify = DB64_OpenPackage(temporary, error, errorSize);
        ok = verify != NULL;
        if (verify)
        {
            DB64_ClosePackage(verify);
        }
    }
    if (ok && !MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        ok = DB64_Error(error, errorSize, "Cannot replace output package: %s", path);
    }
    if (!ok && out)
    {
        DeleteFileA(temporary);
    }
    return ok;
}

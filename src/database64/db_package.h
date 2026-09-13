#pragma once
#include <stddef.h>
#include <stdint.h>

// Stage-one packages hold converted/raw asset files, not the prelinked stream
// ABI in fastfile_format.h. Distinct magic prevents interpreting one as the other.
#define DB64_PACKAGE_MAGIC "KIWIPK64"
#define DB64_PACKAGE_VERSION 1
#define DB64_PACKAGE_MAX_FILES 1000000
#define DB64_PACKAGE_PATH 256

struct DB64PackageHeader
{
    char magic[8];
    uint32_t version;
    uint32_t pointerSize;
    uint64_t fileCount;
    uint64_t directoryOffset;
    uint64_t fileSize;
    char mapName[64];
    uint32_t flags;
    uint32_t reserved;
};

struct DB64PackageEntry
{
    char name[DB64_PACKAGE_PATH];
    uint64_t offset;
    uint64_t storedSize;
    uint64_t size;
    uint32_t compression;
    uint32_t crc;
};

static_assert(sizeof(DB64PackageHeader) == 112);
static_assert(sizeof(DB64PackageEntry) == 288);

struct DB64Package;
struct DB64PackageInput
{
    const char *name;
    const char *path;
};

// Callers serialize access to a package. Returned data lives until ClosePackage.
bool DB64_NormalizePath(const char *path, char *out, size_t capacity);
bool DB64_ValidMapName(const char *name);
DB64Package *DB64_OpenPackage(const char *path, char *error, size_t errorSize);
void DB64_ClosePackage(DB64Package *package);
const DB64PackageHeader *DB64_PackageHeader(const DB64Package *package);
const DB64PackageEntry *DB64_PackageEntryAt(const DB64Package *package, uint64_t index);
int64_t DB64_FindPackageFile(const DB64Package *package, const char *name);
const uint8_t *DB64_ReadPackageFile(DB64Package *package, uint64_t index, char *error, size_t errorSize);
bool DB64_VerifyPackage(DB64Package *package, char *error, size_t errorSize);
bool DB64_WritePackage(const char *path, const char *mapName, const DB64PackageInput *inputs, size_t count,
                       bool compress, char *error, size_t errorSize);

struct DB64FileView
{
    const uint8_t *data;
    size_t size;
    void *owner;
};

// Mount replacement is atomic; open file views pin the previous map's memory.
bool DB64_MountPackage(const char *path, const char *expectedMap, char *error, size_t errorSize);
void DB64_UnmountPackage();
bool DB64_PackageMounted(const char *mapName);
int DB64_AcquireFile(const char *name, DB64FileView *view, char *error, size_t errorSize);
void DB64_ReleaseFile(DB64FileView *view);
bool DB64_PackageFileExists(const char *name);
void DB64_EnumPackageFiles(void (*callback)(const char *name, void *context), void *context);
size_t DB64_ReadView(const DB64FileView *view, size_t *position, void *buffer, size_t size);
// Engine seek origins, matching FileWrapper_Seek: 0=current, 1=end, 2=start.
int DB64_SeekView(const DB64FileView *view, size_t *position, int64_t offset, int origin);

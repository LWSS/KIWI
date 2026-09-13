#define NOMINMAX
#include "db_package.h"
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct DB64Mount
{
    DB64Package *package;
    size_t references;
};

static SRWLOCK s_lock = SRWLOCK_INIT;
static DB64Mount *s_mount;

static void ReleaseMount(DB64Mount *mount)
{
    if (mount && --mount->references == 0)
    {
        DB64_ClosePackage(mount->package);
        free(mount);
    }
}

bool DB64_MountPackage(const char *path, const char *expectedMap, char *error, size_t errorSize)
{
    DB64Package *package = DB64_OpenPackage(path, error, errorSize);
    if (!package)
    {
        return false;
    }
    if (strcmp(DB64_PackageHeader(package)->mapName, expectedMap))
    {
        snprintf(error, errorSize, "Package map name does not match %s", expectedMap);
        DB64_ClosePackage(package);
        return false;
    }
    DB64Mount *mount = (DB64Mount *)calloc(1, sizeof(DB64Mount));
    if (!mount)
    {
        snprintf(error, errorSize, "Out of memory mounting package");
        DB64_ClosePackage(package);
        return false;
    }
    mount->package = package;
    mount->references = 1;
    AcquireSRWLockExclusive(&s_lock);
    DB64Mount *previous = s_mount;
    s_mount = mount;
    ReleaseMount(previous);
    ReleaseSRWLockExclusive(&s_lock);
    return true;
}

void DB64_UnmountPackage()
{
    AcquireSRWLockExclusive(&s_lock);
    DB64Mount *previous = s_mount;
    s_mount = NULL;
    ReleaseMount(previous);
    ReleaseSRWLockExclusive(&s_lock);
}

bool DB64_PackageMounted(const char *name)
{
    AcquireSRWLockShared(&s_lock);
    const bool mounted = s_mount && (!name || !strcmp(DB64_PackageHeader(s_mount->package)->mapName, name));
    ReleaseSRWLockShared(&s_lock);
    return mounted;
}

int DB64_AcquireFile(const char *name, DB64FileView *view, char *error, size_t errorSize)
{
    memset(view, 0, sizeof(DB64FileView));
    AcquireSRWLockExclusive(&s_lock);
    const int64_t index = s_mount ? DB64_FindPackageFile(s_mount->package, name) : -1;
    if (index < 0)
    {
        ReleaseSRWLockExclusive(&s_lock);
        return 0;
    }
    view->data = DB64_ReadPackageFile(s_mount->package, index, error, errorSize);
    if (!view->data)
    {
        ReleaseSRWLockExclusive(&s_lock);
        return -1;
    }
    view->size = (size_t)DB64_PackageEntryAt(s_mount->package, index)->size;
    view->owner = s_mount;
    ++s_mount->references;
    ReleaseSRWLockExclusive(&s_lock);
    return 1;
}

void DB64_ReleaseFile(DB64FileView *view)
{
    AcquireSRWLockExclusive(&s_lock);
    ReleaseMount((DB64Mount *)view->owner);
    memset(view, 0, sizeof(DB64FileView));
    ReleaseSRWLockExclusive(&s_lock);
}

bool DB64_PackageFileExists(const char *name)
{
    AcquireSRWLockShared(&s_lock);
    const bool exists = s_mount && DB64_FindPackageFile(s_mount->package, name) >= 0;
    ReleaseSRWLockShared(&s_lock);
    return exists;
}

void DB64_EnumPackageFiles(void (*callback)(const char *, void *), void *context)
{
    AcquireSRWLockShared(&s_lock);
    if (s_mount)
    {
        const uint64_t count = DB64_PackageHeader(s_mount->package)->fileCount;
        for (uint64_t i = 0; i < count; ++i)
        {
            callback(DB64_PackageEntryAt(s_mount->package, i)->name, context);
        }
    }
    ReleaseSRWLockShared(&s_lock);
}

size_t DB64_ReadView(const DB64FileView *view, size_t *position, void *buffer, size_t size)
{
    if (!view->owner || *position > view->size)
    {
        return 0;
    }
    const size_t remaining = view->size - *position;
    const size_t amount = size < remaining ? size : remaining;
    if (buffer)
    {
        memcpy(buffer, view->data + *position, amount);
    }
    *position += amount;
    return amount;
}

int DB64_SeekView(const DB64FileView *view, size_t *position, int64_t offset, int origin)
{
    int64_t base;
    if (!view->owner)
    {
        return -1;
    }
    switch (origin)
    {
    case 0:
        base = (int64_t)*position;
        break;
    case 1:
        base = (int64_t)view->size;
        break;
    case 2:
        base = 0;
        break;
    default:
        return -1;
    }
    if (offset < -base || offset > (int64_t)view->size - base)
    {
        return -1;
    }
    *position = (size_t)(base + offset);
    return 0;
}

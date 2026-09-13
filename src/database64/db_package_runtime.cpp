#include <universal/q_shared.h>
#include <universal/com_files.h>
#include <qcommon/qcommon.h>
#include <win32/win_localize.h>
#include "db_package.h"
#include "db_package_runtime.h"

void DB64_LoadMapPackage(const char *bspName)
{
    if (!Dvar_GetBool("fs_usePackages"))
    {
        DB64_UnmountPackage();
        return;
    }
    char path[DB64_PACKAGE_PATH];
    if (!DB64_NormalizePath(bspName, path, sizeof(path)))
    {
        DB64_UnmountPackage();
        return;
    }
    char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    char *extension = strrchr(name, '.');
    if (!extension || strcmp(extension, ".d3dbsp"))
    {
        DB64_UnmountPackage();
        return;
    }
    *extension = 0;
    if (!DB64_ValidMapName(name))
    {
        DB64_UnmountPackage();
        return;
    }
    if (DB64_PackageMounted(name))
    {
        return;
    }
    char filename[1024];
    char error[1024];
    const char *language = Win_GetLanguage();
    const char *roots[] = {fs_homepath->current.string, fs_basepath->current.string};
    for (unsigned int i = 0; i < ARRAY_COUNT(roots); ++i)
    {
        const char *game = fs_gameDirVar->current.string;
        for (unsigned int location = 0; location < 2; ++location)
        {
            if (location == 0 && !game[0])
            {
                continue;
            }
            const int length = location == 0
                                   ? snprintf(filename, sizeof(filename), "%s/%s/%s.ff", roots[i], game, name)
                                   : snprintf(filename, sizeof(filename), "%s/zone/%s/%s.ff", roots[i], language, name);
            if (length < 0 || length >= sizeof(filename))
            {
                Com_Error(ERR_DROP, "database64 package path is too long");
            }
            FILE *file = fopen(filename, "rb");
            if (!file)
            {
                continue;
            }
            char magic[8];
            const size_t bytes = fread(magic, 1, sizeof(magic), file);
            fclose(file);
            // Retail/prelinked zones do not shadow a developer's loose BSP.
            if (bytes != sizeof(magic) || memcmp(magic, DB64_PACKAGE_MAGIC, sizeof(magic)))
            {
                continue;
            }
            if (!DB64_MountPackage(filename, name, error, sizeof(error)))
            {
                Com_Error(ERR_DROP, "database64: %s", error);
            }
            Com_Printf(CON_CHANNEL_FILES, "database64 mounted %s\n", filename);
            return;
        }
    }
    DB64_UnmountPackage();
}

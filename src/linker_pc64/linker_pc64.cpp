#include "native_sound_alias_files.h"
#define NOMINMAX
#include <windows.h>
#include <database64/db_package.h>
#include "native_manifest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct LinkerInput
{
    char name[DB64_PACKAGE_PATH];
    char *path;
};

static LinkerInput *s_inputs;
static size_t s_count;
static size_t s_capacity;
static char s_error[1024];
static const char *s_map;

static bool AddInput(const char *name, const char *path)
{
    if (s_count == s_capacity)
    {
        size_t capacity = s_capacity ? s_capacity * 2 : 256;
        if (capacity > DB64_PACKAGE_MAX_FILES)
        {
            capacity = DB64_PACKAGE_MAX_FILES;
        }
        if (capacity <= s_count)
        {
            snprintf(s_error, sizeof(s_error), "Too many package inputs");
            return false;
        }
        LinkerInput *inputs = (LinkerInput *)realloc(s_inputs, capacity * sizeof(LinkerInput));
        if (!inputs)
        {
            snprintf(s_error, sizeof(s_error), "Out of memory collecting inputs");
            return false;
        }
        s_inputs = inputs;
        s_capacity = capacity;
    }
    LinkerInput *input = &s_inputs[s_count];
    if (!DB64_NormalizePath(name, input->name, sizeof(input->name)))
    {
        snprintf(s_error, sizeof(s_error), "Invalid or overlong asset path: %.800s", path);
        return false;
    }
    input->path = _strdup(path);
    if (!input->path)
    {
        strcpy_s(s_error, "Out of memory copying input path");
        return false;
    }
    ++s_count;
    return true;
}

// Keep long OS paths out of recursive ScanRaw stack frames.
static __declspec(noinline) HANDLE FindRawDirectory(const char *root, const char *relative, WIN32_FIND_DATAA *data)
{
    char pattern[32768];
    if (snprintf(pattern, sizeof(pattern), "%s/raw/%s*", root, relative) >= sizeof(pattern))
    {
        strcpy_s(s_error, "Raw directory path is too long");
        return INVALID_HANDLE_VALUE;
    }
    HANDLE search = FindFirstFileA(pattern, data);
    if (search == INVALID_HANDLE_VALUE)
    {
        snprintf(s_error, sizeof(s_error), "Cannot enumerate %.800s", pattern);
    }
    return search;
}

static __declspec(noinline) bool AddRawInput(const char *root, const char *name)
{
    char path[32768];
    if (snprintf(path, sizeof(path), "%s/raw/%s", root, name) >= sizeof(path))
    {
        strcpy_s(s_error, "Input path is too long");
        return false;
    }
    return AddInput(name, path);
}

static bool ScanRaw(const char *root, const char *relative)
{
    WIN32_FIND_DATAA data;
    HANDLE search = FindRawDirectory(root, relative, &data);
    if (search == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    bool ok = true;
    do
    {
        if (!strcmp(data.cFileName, ".") || !strcmp(data.cFileName, ".."))
        {
            continue;
        }
        if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        {
            snprintf(s_error, sizeof(s_error), "Resolve symlink/junction before packaging: %.800s", data.cFileName);
            ok = false;
            break;
        }
        char name[DB64_PACKAGE_PATH];
        if (snprintf(name, sizeof(name), "%s%s%s", relative, data.cFileName,
                     data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? "/" : "") >= sizeof(name))
        {
            strcpy_s(s_error, "Asset path exceeds engine path limit");
            ok = false;
            break;
        }
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            // Source-control and conversion scratch directories are never assets.
            if (data.cFileName[0] == '.' || !_stricmp(data.cFileName, "_temp"))
            {
                continue;
            }
            ok = ScanRaw(root, name);
        }
        else
        {
            const char *extension = strrchr(name, '.');
            if (extension &&
                (!_stricmp(extension, ".ff") || !_stricmp(extension, ".iwd") || !_stricmp(extension, ".map") ||
                 !_stricmp(extension, ".bak") || !_stricmp(extension, ".lin") || !_stricmp(extension, ".errlog")))
            {
                continue;
            }
            if (extension && !_stricmp(extension, ".d3dbsp"))
            {
                char expected[DB64_PACKAGE_PATH];
                snprintf(expected, sizeof(expected), "maps/mp/%s.d3dbsp", s_map);
                if (_stricmp(name, expected))
                {
                    snprintf(expected, sizeof(expected), "maps/%s.d3dbsp", s_map);
                    if (_stricmp(name, expected))
                    {
                        continue;
                    }
                }
            }
            ok = AddRawInput(root, name);
        }
    } while (ok && FindNextFileA(search, &data));
    if (ok && GetLastError() != ERROR_NO_MORE_FILES)
    {
        strcpy_s(s_error, "Error enumerating raw assets");
        ok = false;
    }
    FindClose(search);
    return ok;
}

static bool MakeDirectories(const char *path)
{
    char copy[32768];
    if (strlen(path) >= sizeof(copy))
    {
        return false;
    }
    strcpy_s(copy, path);
    char *begin = copy + 1;
    if ((copy[0] == '/' || copy[0] == '\\') && copy[1] == copy[0])
    {
        // A UNC server/share is a root, not a directory to create.
        begin = copy + 2;
        for (int component = 0; component < 2; ++component)
        {
            while (*begin && *begin != '/' && *begin != '\\')
            {
                ++begin;
            }
            if (*begin)
            {
                ++begin;
            }
        }
    }
    for (char *p = begin; *p; ++p)
    {
        if (*p == '/' || *p == '\\')
        {
            const char saved = *p;
            *p = 0;
            const DWORD attributes = GetFileAttributesA(copy);
            if ((attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) &&
                !CreateDirectoryA(copy, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
            {
                return false;
            }
            *p = saved;
        }
    }
    return true;
}

static int Verify(const char *path)
{
    DB64Package *package = DB64_OpenPackage(path, s_error, sizeof(s_error));
    if (!package)
    {
        fprintf(stderr, "ERROR: %s\n", s_error);
        return 1;
    }
    const DB64PackageHeader *header = DB64_PackageHeader(package);
    int result = 0;
    if (!DB64_VerifyPackage(package, s_error, sizeof(s_error)))
    {
        fprintf(stderr, "ERROR: %s\n", s_error);
        result = 1;
    }
    if (!result)
    {
        printf("Verified %llu files; map %s; %llu bytes.\n", (unsigned long long)header->fileCount, header->mapName,
               (unsigned long long)header->fileSize);
    }
    DB64_ClosePackage(package);
    return result;
}

static void Usage()
{
    puts("KIWI linker_pc64 - map packages and native asset compilation\n"
         "Usage: linker_pc64 [-root game_path] [-language english] [-compress|-nocompress]\n"
         "                   [-cleanup] [-output file.ff] map_name\n"
         "       linker_pc64 -verify file.ff\n\n"
         "       linker_pc64 -native zone_source.csv -root game_path -output assets.ff zone_name\n"
         "       linker_pc64 -native-map maps/mp/map_name.d3dbsp [-native extra_assets.csv] map_name\n"
         "Native CSV importers: rawfile, image, material, xmodel, xanim, fx, loaded_sound (PCM WAV), soundcurve, physpreset, mapworlds, gfxworld, map.\n"
         "xmodel resolves LODs, materials, presets and box/cylinder/convex-brush physics maps.\n"
         "sound,alias_name (or sound,table.csv/alias_name) imports a loaded PCM alias and its dependencies.\n"
         "Native dependencies use loose raw files, then unlocalized raw/main IWDs; water grids support 4..64.\n"
         "mapworlds imports entities/lights/MP metadata; gfxworld imports a BSP render world and its dependencies.\n"
         "map imports render/collision worlds plus entities/lights/MP metadata from one BSP.\n"
         "Default package mode reads game_path/raw; writes game_path/zone/language/map_name.ff.\n"
         "Contains exactly one BSP and all other raw assets, conservatively retaining\n"
         "dynamic script/material/sound dependencies. Other BSPs and tool outputs are excluded.\n"
         "Package mode retains runtime raw parsing. Native mode has limited importers so far.\n"
         "Automatic dependency discovery and retail .ff input are not supported yet.\n"
         "-cleanup is accepted for modtools command compatibility; source files are never removed.");
}

static int LinkerMain(int argc, char **argv)
{
    static_assert(sizeof(void *) == 8, "linker_pc64 requires x64");
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *root = NULL;
    const char *language = "english";
    const char *output = NULL;
    const char *nativeManifest = NULL;
    const char *nativeMap = NULL;
    bool compress = true;
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "-help") || !strcmp(argv[i], "--help"))
        {
            Usage();
            return 0;
        }
        if (!strcmp(argv[i], "-verify"))
        {
            if (argc != 3)
            {
                Usage();
                return 1;
            }
            return Verify(argv[2]);
        }
        if (!strcmp(argv[i], "-root") || !strcmp(argv[i], "-language") || !strcmp(argv[i], "-output") ||
            !strcmp(argv[i], "-native") || !strcmp(argv[i], "-native-map"))
        {
            const char *option = argv[i++];
            if (i == argc)
            {
                fprintf(stderr, "ERROR: Missing value for %s\n", option);
                return 1;
            }
            if (!strcmp(option, "-root"))
            {
                root = argv[i];
            }
            else if (!strcmp(option, "-language"))
            {
                language = argv[i];
            }
            else if (!strcmp(option, "-native"))
            {
                nativeManifest = argv[i];
            }
            else if (!strcmp(option, "-native-map"))
            {
                nativeMap = argv[i];
            }
            else
            {
                output = argv[i];
            }
        }
        else if (!strcmp(argv[i], "-compress"))
        {
            compress = true;
        }
        else if (!strcmp(argv[i], "-nocompress"))
        {
            compress = false;
        }
        else if (!strcmp(argv[i], "-cleanup"))
        {
            // Writes are transactional; there is no source cleanup to perform.
        }
        else if (argv[i][0] == '-' || s_map)
        {
            fprintf(stderr, "ERROR: Unknown option or second map: %s\n", argv[i]);
            return 1;
        }
        else
        {
            s_map = argv[i];
        }
    }
    if (!DB64_ValidMapName(s_map) || !DB64_ValidMapName(language))
    {
        fputs("ERROR: Invalid map name or language\n", stderr);
        Usage();
        return 1;
    }
    char current[32768];
    if (!root)
    {
        if (!GetCurrentDirectoryA(sizeof(current), current))
        {
            return 1;
        }
        char raw[32768];
        snprintf(raw, sizeof(raw), "%s/raw", current);
        if (GetFileAttributesA(raw) == INVALID_FILE_ATTRIBUTES)
        {
            char *slash = strrchr(current, '\\');
            if (slash && !_stricmp(slash + 1, "bin"))
            {
                *slash = 0;
            }
        }
        root = current;
    }
    char manifestPath[32768];
    if (nativeManifest && nativeManifest[0] != '/' && nativeManifest[0] != '\\' &&
        !(nativeManifest[0] && nativeManifest[1] == ':'))
    {
        const int length = snprintf(manifestPath, sizeof(manifestPath), "%s/%s", root, nativeManifest);
        if (length < 0 || length >= sizeof(manifestPath))
        {
            fputs("ERROR: Manifest path is too long\n", stderr);
            return 1;
        }
        nativeManifest = manifestPath;
    }
    if (nativeMap)
    {
        char normalized[DB64_PACKAGE_PATH], expected[DB64_PACKAGE_PATH];
        snprintf(expected, sizeof(expected), "%s.d3dbsp", s_map);
        if (!DB64_NormalizePath(nativeMap, normalized, sizeof(normalized)))
        {
            fputs("ERROR: Invalid native map path\n", stderr);
            return 1;
        }
        const char *name = strrchr(normalized, '/');
        if (_stricmp(name ? name + 1 : normalized, expected))
        {
            fputs("ERROR: Native BSP name does not match the output map name\n", stderr);
            return 1;
        }
    }
    char destination[32768];
    if (nativeMap)
    {
        if (!output)
        {
            const int length = snprintf(destination, sizeof(destination), "%s/zone/%s/%s.ff", root, language, s_map);
            if (length < 0 || length >= sizeof(destination))
            {
                fputs("ERROR: Output path is too long\n", stderr);
                return 1;
            }
            output = destination;
        }
        if (!MakeDirectories(output) ||
            !Linker_CompileNativeMap(root, nativeMap, nativeManifest, output, compress, s_error, sizeof(s_error), language))
        {
            fprintf(stderr, "ERROR: %s\n", s_error[0] ? s_error : "Cannot create output directory");
            return 1;
        }
        return 0;
    }
    if (nativeManifest)
    {
        if (!output)
        {
            fputs("ERROR: -native currently requires an explicit -output path\n", stderr);
            return 1;
        }
        if (!MakeDirectories(output) ||
            !Linker_CompileNativeManifest(root, nativeManifest, output, compress, s_error, sizeof(s_error), s_map, language))
        {
            fprintf(stderr, "ERROR: %s\n", s_error[0] ? s_error : "Cannot create output directory");
            return 1;
        }
        return 0;
    }
    if (!output)
    {
        if (snprintf(destination, sizeof(destination), "%s/zone/%s/%s.ff", root, language, s_map) >=
            sizeof(destination))
        {
            fputs("ERROR: Output path is too long\n", stderr);
            return 1;
        }
        output = destination;
    }
    printf("Map: %s\nCollecting raw assets from %s/raw\n", s_map, root);
    bool ok = ScanRaw(root, "");
    DB64PackageInput *inputs = NULL;
    if (ok)
    {
        inputs = (DB64PackageInput *)calloc(s_count, sizeof(DB64PackageInput));
        if (!inputs)
        {
            strcpy_s(s_error, "Out of memory preparing package");
            ok = false;
        }
    }
    if (ok)
    {
        for (size_t i = 0; i < s_count; ++i)
        {
            inputs[i].name = s_inputs[i].name;
            inputs[i].path = s_inputs[i].path;
        }
        printf("Building %zu files into %s\n", s_count, output);
        if (!MakeDirectories(output))
        {
            strcpy_s(s_error, "Cannot create output directory");
            ok = false;
        }
        else
        {
            ok = DB64_WritePackage(output, s_map, inputs, s_count, compress, s_error, sizeof(s_error));
        }
    }
    free(inputs);
    for (size_t i = 0; i < s_count; ++i)
    {
        free(s_inputs[i].path);
    }
    free(s_inputs);
    if (!ok)
    {
        fprintf(stderr, "ERROR: %s\n", s_error);
        return 1;
    }
    puts("Package written successfully.");
    return 0;
}

int main(int argc, char **argv)
{
    try
    {
        return LinkerMain(argc, argv);
    }
    catch (...)
    {
        Linker_EndSoundTableCache();
        fprintf(stderr, "ERROR: Native asset compiler aborted; see the preceding diagnostic.\n");
        return 1;
    }
}

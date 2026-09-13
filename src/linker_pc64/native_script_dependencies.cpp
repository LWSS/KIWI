#include "native_script_dependencies.h"
#include <database64/db_package.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static bool Word(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '\\' || c == '/' || c == '.';
}

static bool AddScript(const char *name, LinkerScriptDependency add, void *context)
{
    char path[DB64_PACKAGE_PATH], normalized[DB64_PACKAGE_PATH];
    const size_t length = strlen(name);
    const bool extension = length >= 4 && !_stricmp(name + length - 4, ".gsc");
    const int count = snprintf(path, sizeof(path), "%s%s", name, extension ? "" : ".gsc");
    return count > 0 && count < sizeof(path) && DB64_NormalizePath(path, normalized, sizeof(normalized)) &&
           add(normalized, context);
}

bool Linker_ScanScriptAssetDependencies(const void *data, size_t size, LinkerScriptDependency add,
                                        LinkerScriptAssetDependency addAsset, void *context,
                                        char *error, size_t errorSize)
{
    const char *text = (const char *)data;
    size_t cursor = 0;
    char previous[DB64_PACKAGE_PATH] = {};
    char literal[DB64_PACKAGE_PATH] = {};
    const char *assetType = NULL;
    bool haveLiteral = false, qualified = false, qualifyNext = false;
    bool directive = false, include = false, valid = data && add && !memchr(data, 0, size);
    while (valid && cursor < size)
    {
        const char c = text[cursor];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            ++cursor;
            continue;
        }
        if (c == '/' && cursor + 1 < size && text[cursor + 1] == '/')
        {
            cursor += 2;
            while (cursor < size && text[cursor] != '\n')
            {
                ++cursor;
            }
            continue;
        }
        if (c == '/' && cursor + 1 < size && text[cursor + 1] == '*')
        {
            cursor += 2;
            while (cursor + 1 < size && !(text[cursor] == '*' && text[cursor + 1] == '/'))
            {
                ++cursor;
            }
            valid = cursor + 1 < size;
            cursor += valid ? 2 : 0;
            continue;
        }
        if (c == '"')
        {
            ++cursor;
            bool closed = false;
            size_t length = 0;
            while (cursor < size)
            {
                char value = text[cursor++];
                if (value == '"')
                {
                    closed = true;
                    break;
                }
                if (value == '\\' && cursor < size)
                {
                    value = text[cursor++];
                }
                if (assetType)
                {
                    if (length + 1 >= sizeof(literal))
                    {
                        break;
                    }
                    literal[length++] = value;
                }
            }
            literal[length] = 0;
            haveLiteral = assetType != NULL;
            valid = closed && !include;
            previous[0] = 0;
            directive = false;
            continue;
        }
        if (Word((unsigned char)c))
        {
            assetType = NULL;
            haveLiteral = false;
            qualified = qualifyNext;
            qualifyNext = false;
            const size_t start = cursor++;
            while (cursor < size && Word((unsigned char)text[cursor]))
            {
                if (text[cursor] == '/' && cursor + 1 < size &&
                    (text[cursor + 1] == '/' || text[cursor + 1] == '*'))
                {
                    break;
                }
                ++cursor;
            }
            const size_t length = cursor - start;
            if (length >= sizeof(previous))
            {
                valid = false;
                break;
            }
            memcpy(previous, text + start, length);
            previous[length] = 0;
            if (include)
            {
                valid = AddScript(previous, add, context);
                include = false;
            }
            else
            {
                include = directive && !strcmp(previous, "include");
            }
            directive = false;
            continue;
        }
        if (c == ':' && cursor + 1 < size && text[cursor + 1] == ':' && previous[0])
        {
            valid = AddScript(previous, add, context);
            qualifyNext = true;
            cursor += 2;
        }
        else
        {
            valid = !include;
            ++cursor;
        }
        if (valid && assetType && haveLiteral && addAsset &&
            (c == ')' || (c == ',' && !strcmp(assetType, "sound"))))
        {
            char normalized[DB64_PACKAGE_PATH];
            valid = DB64_NormalizePath(literal, normalized, sizeof(normalized)) && addAsset(assetType, normalized, context);
        }
        assetType = NULL;
        haveLiteral = false;
        if (c == '(' && !qualified && addAsset)
        {
            if (!_stricmp(previous, "loadfx"))
            {
                assetType = "fx";
            }
            else if (!_stricmp(previous, "precachemodel"))
            {
                assetType = "xmodel";
            }
            else if (!_stricmp(previous, "precacheshader"))
            {
                assetType = "material";
            }
            else if (!_stricmp(previous, "playsound") || !_stricmp(previous, "playsoundasmaster") ||
                     !_stricmp(previous, "playsoundtoteam") || !_stricmp(previous, "playsoundtoplayer") ||
                     !_stricmp(previous, "playloopsound") || !_stricmp(previous, "playlocalsound") ||
                     !_stricmp(previous, "ambientplay"))
            {
                assetType = "sound";
            }
        }
        directive = c == '#';
        previous[0] = 0;
    }
    valid = valid && !include;
    if (!valid && errorSize)
    {
        snprintf(error, errorSize, "Cannot discover script dependencies at byte %zu", cursor);
    }
    return valid;
}

bool Linker_ScanScriptDependencies(const void *data, size_t size, LinkerScriptDependency add, void *context,
                                   char *error, size_t errorSize)
{
    return Linker_ScanScriptAssetDependencies(data, size, add, NULL, context, error, errorSize);
}

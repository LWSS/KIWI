#include <universal/q_shared.h>
#include <database64/database.h>
#include "native_localize.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void Linker_FreeLocalization(LinkerLocalization *source)
{
    if (source)
    {
        for (int i = 0; i < source->count; ++i)
        {
            free((void *)source->assets[i].header.localize->name);
            free(source->assets[i].header.localize);
        }
        free(source->assets);
        free(source->text);
        free(source);
    }
}

static int CompareLocalization(const void *left, const void *right)
{
    return strcmp(((const XAsset *)left)->header.localize->name,
                  ((const XAsset *)right)->header.localize->name);
}

static bool ValidFormat(const char *value)
{
    bool arguments[9] = {};
    while ((value = strstr(value, "&&")) != NULL)
    {
        value += 2;
        if (*value < '1' || *value > '9' || arguments[*value - '1'])
        {
            return false;
        }
        arguments[*value++ - '1'] = true;
    }
    return true;
}

bool Linker_ImportLocalization(const void *data, size_t size, const char *name, const char *language,
                               LinkerLocalization **result, char *error, size_t errorSize)
{
    if (!result || (!error && errorSize))
    {
        return false;
    }
    *result = NULL;
    if (!data || !size || size > 16 * 1024 * 1024 || memchr(data, 0, size) ||
        !name || !name[0] || strlen(name) >= 256 || !language || !language[0])
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid localization source");
        }
        return false;
    }
    char package[256];
    const char *base = name;
    for (const char *p = name; *p; ++p)
    {
        if (*p == '/' || *p == '\\')
        {
            base = p + 1;
        }
    }
    strcpy(package, base);
    char *extension = strrchr(package, '.');
    if (extension)
    {
        *extension = 0;
    }
    for (char *p = package; *p; ++p)
    {
        if (*p >= 'a' && *p <= 'z')
        {
            *p += 'A' - 'a';
        }
    }
    LinkerLocalization *source = (LinkerLocalization *)calloc(1, sizeof(LinkerLocalization));
    if (!source)
    {
        return false;
    }
    source->assets = (XAsset *)calloc(32768, sizeof(XAsset));
    source->text = (char *)malloc(size + 1);
    bool valid = source->assets && source->text && package[0];
    if (valid)
    {
        memcpy(source->text, data, size);
        source->text[size] = 0;
    }
    char *cursor = source->text;
    const char *english = NULL, *translation = NULL;
    bool version = false, ended = false;
    unsigned int lineNumber = 0;
    while (valid && *cursor)
    {
        ++lineNumber;
        char *line = cursor;
        while (*cursor && *cursor != '\n' && *cursor != '\r')
        {
            ++cursor;
        }
        if (*cursor)
        {
            const char terminator = *cursor;
            *cursor++ = 0;
            if (terminator == '\r' && *cursor == '\n')
            {
                ++cursor;
            }
        }
        bool quoted = false;
        for (char *p = line; *p; ++p)
        {
            if (*p == '"')
            {
                quoted = !quoted;
            }
            if (!quoted && p[0] == '/' && p[1] == '/')
            {
                *p = 0;
                break;
            }
        }
        while (*line == ' ' || *line == '\t')
        {
            ++line;
        }
        char *end = line + strlen(line);
        while (end > line && (end[-1] == ' ' || end[-1] == '\t'))
        {
            *--end = 0;
        }
        if (!*line)
        {
            continue;
        }
        if (ended || strlen(line) >= 16388)
        {
            valid = false;
            break;
        }
        char *value = line;
        while (*value && *value != ' ' && *value != '\t')
        {
            ++value;
        }
        if (*value)
        {
            *value++ = 0;
        }
        while (*value == ' ' || *value == '\t')
        {
            ++value;
        }
        if (*value == '"')
        {
            ++value;
            end = value + strlen(value);
            if (end == value || end[-1] != '"')
            {
                valid = false;
                break;
            }
            end[-1] = 0;
        }
        if (!_stricmp(line, "VERSION"))
        {
            valid = !version && !strcmp(value, "1");
            version = true;
        }
        else if (!_stricmp(line, "CONFIG") || !_stricmp(line, "FILENOTES") ||
                 !_stricmp(line, "NOTES") || !_stricmp(line, "FLAGS"))
        {
            continue;
        }
        else if (!_stricmp(line, "REFERENCE") || !_stricmp(line, "ENDMARKER"))
        {
            if (source->count)
            {
                if (!english)
                {
                    valid = false;
                    break;
                }
                source->assets[source->count - 1].header.localize->value = translation ? translation : english;
            }
            if (!_stricmp(line, "ENDMARKER"))
            {
                ended = !*value;
                valid = ended;
                continue;
            }
            if (!version || !*value || strlen(value) + strlen(package) + 2 > 256 || source->count == 32768)
            {
                valid = false;
                break;
            }
            LocalizeEntry *entry = (LocalizeEntry *)calloc(1, sizeof(LocalizeEntry));
            char *key = (char *)malloc(strlen(package) + strlen(value) + 2);
            if (!entry || !key)
            {
                free(entry);
                free(key);
                valid = false;
                break;
            }
            sprintf(key, "%s_%s", package, value);
            entry->name = key;
            source->assets[source->count].type = ASSET_TYPE_LOCALIZE_ENTRY;
            source->assets[source->count++].header.localize = entry;
            english = translation = NULL;
        }
        else if (!_strnicmp(line, "LANG_", 5) && line[5] && source->count)
        {
            // StringEd only converts literal backslash-n; other backslashes survive.
            char *output = value;
            for (char *p = value; *p; ++p)
            {
                if (p[0] == '\\' && p[1] == 'n')
                {
                    *output++ = '\n';
                    ++p;
                }
                else
                {
                    *output++ = *p;
                }
            }
            *output = 0;
            valid = ValidFormat(value);
            if (!_stricmp(line + 5, "english"))
            {
                valid = valid && !english;
                english = value;
            }
            else if (!_stricmp(line + 5, language))
            {
                translation = _stricmp(value, "#same") ? value : NULL;
            }
        }
        else
        {
            valid = false;
        }
    }
    valid = valid && version && ended;
    if (valid)
    {
        qsort(source->assets, source->count, sizeof(XAsset), CompareLocalization);
        for (int i = 1; i < source->count; ++i)
        {
            if (!strcmp(source->assets[i - 1].header.localize->name, source->assets[i].header.localize->name))
            {
                valid = false;
                break;
            }
        }
    }
    if (!valid)
    {
        Linker_FreeLocalization(source);
        if (errorSize)
        {
            snprintf(error, errorSize, "Malformed localization source %s at line %u", name, lineNumber);
        }
        return false;
    }
    *result = source;
    return true;
}

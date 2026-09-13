#include <universal/q_shared.h>
#include "native_sun.h"
#include "native_asset_files.h"
#include "native_material_files.h"
#include <database64/db_package.h>
#include <gfx_d3d/r_image.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char *names[21] = {"r_sunsprite_shader",    "r_sunsprite_size",     "r_sunflare_shader",
                                "r_sunflare_min_size",   "r_sunflare_min_angle", "r_sunflare_max_size",
                                "r_sunflare_max_angle",  "r_sunflare_max_alpha", "r_sunflare_fadein",
                                "r_sunflare_fadeout",    "r_sunblind_min_angle", "r_sunblind_max_angle",
                                "r_sunblind_max_darken", "r_sunblind_fadein",    "r_sunblind_fadeout",
                                "r_sunglare_min_angle",  "r_sunglare_max_angle", "r_sunglare_max_lighten",
                                "r_sunglare_fadein",     "r_sunglare_fadeout",   "r_sun_fx_position"};

static bool Token(const char **cursor, const char *end, char *out, size_t capacity)
{
    const char *p = *cursor;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r'))
    {
        ++p;
    }
    const bool quoted = p < end && *p == '"';
    if (quoted)
    {
        ++p;
    }
    size_t count = 0;
    while (p < end && (quoted ? *p != '"' : *p != ' ' && *p != '\t' && *p != '\r'))
    {
        if (count + 1 >= capacity || !*p)
        {
            return false;
        }
        out[count++] = *p++;
    }
    if (quoted && (p == end || *p++ != '"'))
    {
        return false;
    }
    out[count] = 0;
    *cursor = p;
    return count != 0;
}

bool Linker_ParseSunSettings(const void *data, size_t size, LinkerSunSettings *settings, char *error, size_t errorSize)
{
    if (!settings || (!error && errorSize))
    {
        return false;
    }
    bool valid = data && size && size <= 65536 && !memchr(data, 0, size);
    const char *cursor = (const char *)data, *end = valid ? cursor + size : cursor;
    LinkerSunSettings result = {};
    bool seen[21] = {};
    double values[21] = {}, angles[3] = {};
    while (valid && cursor < end)
    {
        const char *lineEnd = (const char *)memchr(cursor, '\n', end - cursor);
        if (!lineEnd)
        {
            lineEnd = end;
        }
        const char *p = cursor;
        cursor = lineEnd < end ? lineEnd + 1 : end;
        while (p < lineEnd && (*p == ' ' || *p == '\t' || *p == '\r'))
        {
            ++p;
        }
        if (p == lineEnd || (lineEnd - p >= 2 && p[0] == '/' && p[1] == '/'))
        {
            continue;
        }
        char key[128], value[1024];
        valid = Token(&p, lineEnd, key, sizeof(key));
        if (!valid)
        {
            break;
        }
        unsigned int index = 0;
        while (index < 21 && _stricmp(key, names[index]))
        {
            ++index;
        }
        if (!valid || index == 21)
        {
            continue;
        }
        valid = Token(&p, lineEnd, value, sizeof(value));
        if (!valid)
        {
            break;
        }
        if (index == 0 || index == 2)
        {
            strcpy(index == 0 ? result.sprite : result.flare, value);
        }
        else
        {
            char *number = value;
            for (unsigned int j = 0; valid && j < (index == 20 ? 3 : 1); ++j)
            {
                char *next;
                const double numberValue = strtod(number, &next);
                const double maximum =
                    index == 20 ? 360
                                : (index == 1 ? 1000
                                              : (index == 3 || index == 5
                                                     ? 10000
                                                     : (index == 4 || index == 6 || index == 10 || index == 11 ||
                                                                index == 15 || index == 16
                                                            ? 90
                                                            : (index == 7 || index == 12 || index == 17 ? 1 : 60))));
                valid = next != number && isfinite(numberValue) &&
                        numberValue >= (index == 20 ? -360 : (index == 1 ? 1 : 0)) && numberValue <= maximum;
                if (index == 20)
                {
                    angles[j] = numberValue;
                }
                else
                {
                    values[index] = numberValue;
                }
                number = next;
            }
            while (*number == ' ' || *number == '\t' || *number == '\r')
            {
                ++number;
            }
            valid = valid && !*number;
        }
        seen[index] = true;
    }
    for (unsigned int i = 0; i < 21; ++i)
    {
        valid = valid && seen[i];
    }
    if (!valid)
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid or incomplete sun file: all 21 sun settings are required");
        }
        return false;
    }
    const double radians = 0.017453292519943295;
    sunflare_t *sun = &result.sun;
    sun->spriteSize = (float)values[1];
    sun->flareMinSize = (float)(values[3] * 0.5);
    sun->flareMinDot = (float)cos(values[4] * radians);
    sun->flareMaxSize = (float)(values[5] * 0.5);
    sun->flareMaxDot = (float)cos(values[6] * radians);
    sun->flareMaxAlpha = (float)values[7];
    sun->flareFadeInTime = (int)nearbyint((float)values[8] * 1000.0f);
    sun->flareFadeOutTime = (int)nearbyint((float)values[9] * 1000.0f);
    sun->blindMinDot = (float)cos(values[10] * radians);
    sun->blindMaxDot = (float)cos(values[11] * radians);
    sun->blindMaxDarken = (float)values[12];
    sun->blindFadeInTime = (int)nearbyint((float)values[13] * 1000.0f);
    sun->blindFadeOutTime = (int)nearbyint((float)values[14] * 1000.0f);
    sun->glareMinDot = (float)cos(values[15] * radians);
    sun->glareMaxDot = (float)cos(values[16] * radians);
    sun->glareMaxLighten = (float)values[17];
    sun->glareFadeInTime = (int)nearbyint((float)values[18] * 1000.0f);
    sun->glareFadeOutTime = (int)nearbyint((float)values[19] * 1000.0f);
    sun->sunFxPosition[0] = (float)(cos(angles[0] * radians) * cos(angles[1] * radians));
    sun->sunFxPosition[1] = (float)(cos(angles[0] * radians) * sin(angles[1] * radians));
    sun->sunFxPosition[2] = (float)-sin(angles[0] * radians);
    // Dependency compilation sets hasValidData after both material pointers have been resolved.
    *settings = result;
    return true;
}

void Linker_FreeCompiledSun(LinkerCompiledSun *sun)
{
    if (sun)
    {
        if (sun->flare != sun->sprite)
        {
            Linker_FreeCompiledMaterial(sun->flare);
        }
        Linker_FreeCompiledMaterial(sun->sprite);
        free(sun);
    }
}

bool Linker_CompileSun(const char *root, const char *mapName, LinkerCompiledSun **result, char *error, size_t errorSize)
{
    if (!result || (!error && errorSize))
    {
        return false;
    }
    *result = NULL;
    if (errorSize)
    {
        error[0] = 0;
    }
    char normalized[DB64_PACKAGE_PATH], path[DB64_PACKAGE_PATH];
    bool valid = root && mapName && DB64_NormalizePath(mapName, normalized, sizeof(normalized));
    if (valid)
    {
        char *base = strrchr(normalized, '/');
        base = base ? base + 1 : normalized;
        char *extension = strchr(base, '.');
        if (extension)
        {
            *extension = 0;
        }
        const int length = snprintf(path, sizeof(path), "sun/%s.sun", base);
        valid = *base && length > 0 && length < sizeof(path);
    }
    void *data = NULL;
    size_t size = 0;
    bool found = false;
    if (valid && !Linker_ReadRawAssetFile(root, path, &data, &size, 65536, &found))
    {
        if (!found)
        {
            return true;
        }
        valid = false;
    }
    LinkerSunSettings settings = {};
    valid = valid && Linker_ParseSunSettings(data, size, &settings, error, errorSize);
    free(data);
    LinkerCompiledSun *sun = valid ? (LinkerCompiledSun *)calloc(1, sizeof(LinkerCompiledSun)) : NULL;
    valid = valid && sun;
    if (valid)
    {
        sun->sun = settings.sun;
        valid = Linker_CompileMaterialFiles(root, settings.sprite, IMAGE_TRACK_FX, &sun->sprite, error, errorSize);
        if (valid && !_stricmp(settings.sprite, settings.flare))
        {
            sun->flare = sun->sprite;
        }
        else if (valid)
        {
            valid = Linker_CompileMaterialFiles(root, settings.flare, IMAGE_TRACK_FX, &sun->flare, error, errorSize);
        }
        if (valid)
        {
            sun->sun.spriteMaterial = sun->sprite->material;
            sun->sun.flareMaterial = sun->flare->material;
            sun->sun.hasValidData = true;
        }
    }
    if (!valid)
    {
        Linker_FreeCompiledSun(sun);
        if (errorSize && !error[0])
        {
            snprintf(error, errorSize, "Cannot compile sun file for map '%s'", mapName ? mapName : "");
        }
        return false;
    }
    *result = sun;
    return true;
}

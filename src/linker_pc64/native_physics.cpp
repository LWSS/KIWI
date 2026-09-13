#include <universal/q_shared.h>
#include <physics/phys_local.h>
#include "native_physics.h"
#include <stdlib.h>
#include <stdio.h>
#include <float.h>
#include <math.h>
#include <errno.h>

static bool Fail(char *error, size_t size, const char *message)
{
    if (size)
    {
        snprintf(error, size, "%s", message);
    }
    return false;
}

bool Linker_ImportPhysicsPreset(const void *data, size_t size, const char *name, PhysPreset **result, char *error,
                                size_t errorSize)
{
    if (!result || (!error && errorSize))
    {
        return false;
    }
    *result = NULL;
    if (!data || size < 6 || size >= 8198 || !name || !name[0] || memcmp(data, "PHYSIC", 6) || memchr(data, 0, size))
    {
        return Fail(error, errorSize, "Invalid PHYSIC preset input");
    }
    const size_t nameSize = strlen(name) + 1;
    if (nameSize > 32768)
    {
        return Fail(error, errorSize, "Physics preset name is too long");
    }
    PhysPreset *preset = (PhysPreset *)calloc(1, sizeof(PhysPreset) + nameSize + size + 1);
    if (!preset)
    {
        return Fail(error, errorSize, "Out of memory importing physics preset");
    }
    char *ownedName = (char *)(preset + 1);
    memcpy(ownedName, name, nameSize);
    preset->name = ownedName;
    char *text = ownedName + nameSize;
    memcpy(text, (const char *)data + 6, size - 6);
    preset->sndAliasPrefix = "";
    const char *fields[] = {"mass",
                            "bounce",
                            "friction",
                            "bulletForceScale",
                            "explosiveForceScale",
                            "piecesSpreadFraction",
                            "piecesUpwardVelocity",
                            "sndAliasPrefix",
                            "isFrictionInfinity",
                            "tempDefaultToCylinder"};
    float *numbers[] = {&preset->mass,
                        &preset->bounce,
                        &preset->friction,
                        &preset->bulletForceScale,
                        &preset->explosiveForceScale,
                        &preset->piecesSpreadFraction,
                        &preset->piecesUpwardVelocity};
    bool seen[10] = {};
    bool infiniteFriction = false;
    bool ok = true;
    char *cursor = text;
    if (*cursor == '\\')
    {
        ++cursor;
    }
    while (*cursor && ok)
    {
        char *key = cursor;
        char *separator = strchr(cursor, '\\');
        if (!separator)
        {
            ok = false;
            break;
        }
        *separator = 0;
        char *value = separator + 1;
        cursor = strchr(value, '\\');
        if (cursor)
        {
            *cursor = 0;
        }
        int field = 0;
        while (field < 10 && _stricmp(key, fields[field]))
        {
            ++field;
        }
        if (field == 10 || seen[field] || strpbrk(value, "\";"))
        {
            ok = false;
        }
        else
        {
            seen[field] = true;
            if (field == 7)
            {
                preset->sndAliasPrefix = value;
            }
            else
            {
                errno = 0;
                char *end;
                const float number = strtof(value, &end);
                const bool parsed = end != value;
                while (*end == ' ' || *end == '\r' || *end == '\n' || *end == '\t')
                {
                    ++end;
                }
                if (!parsed || *end || errno == ERANGE || !isfinite(number) ||
                    (field >= 8 && number != 0 && number != 1))
                {
                    ok = false;
                }
                else if (field < 7)
                {
                    *numbers[field] = number;
                }
                else if (field == 8)
                {
                    infiniteFriction = number != 0;
                }
                else
                {
                    preset->tempDefaultToCylinder = number != 0;
                }
            }
        }
        if (!cursor)
        {
            break;
        }
        ++cursor;
        if (!*cursor)
        {
            ok = false;
        }
    }
    if (!ok)
    {
        free(preset);
        return Fail(error, errorSize, "Invalid or unsupported physics preset field");
    }
    if (infiniteFriction)
    {
        preset->friction = FLT_MAX;
    }
    *result = preset;
    return true;
}

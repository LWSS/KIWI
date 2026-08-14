/*
 * targetplatform.c — Target platform table and selection.
 *
 * Source: targetplatform.cpp (from LST annotation)
 * Maintains a table of supported target platforms ("xenon", "pc", etc.)
 * and the currently-selected platform pointer.
 */

#include "cod2rad64.h"

PlatformEntry_t g_platformTable[3] = {
    { "pc",    1, "pc",    "materials", 0 },
    { "xenon", 0, "xenon", "materials", 1 },
    { "ps3",   2, "ps3",   "materials", 1 },
};
PlatformEntry_t *g_targetPlatform;

static char s_assertDisable_SetTargetPlatformByName_arg;

/*
================
SetTargetPlatformByName

Searches the platform table for a name matching cmdlineSwitch
(case-insensitive). If found, sets g_targetPlatform. Errors if
a different platform was already selected.
Returns 1 on success, 0 if not found or conflict.
================
*/
int SetTargetPlatformByName(const char *cmdlineSwitch)
{
    int i;

    Assert(cmdlineSwitch, s_assertDisable_SetTargetPlatformByName_arg);

    for (i = 0; i < 3; i++)
    {
        if (I_stricmp(cmdlineSwitch, g_platformTable[i].name) == 0)
        {
            if (g_targetPlatform != 0 && g_targetPlatform != &g_platformTable[i])
            {
                Error("Error: Target platform already set as %s, trying to set again as %s\n",
                      g_targetPlatform->name, g_platformTable[i].name);
                return 0;
            }
            g_targetPlatform = &g_platformTable[i];
            return 1;
        }
    }

    return 0;
}

/*
================
ValidatePlatformSet

Checks that g_targetPlatform was set. If not, prints an error
message listing valid platform names and returns 0.
Returns 1 if a platform is set.
================
*/
int ValidatePlatformSet(void)
{
    int i;

    if (g_targetPlatform != 0)
        return 1;

    printf("No platform specified.  '-platform' must be set using one of the following:\n");

    for (i = 0; i < 3; i++)
        printf("  %s\n", g_platformTable[i].name);

    return 0;
}

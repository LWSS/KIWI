/*
targetplatform.c — Platform selection

Reconstructed from cod2map.exe by Rose.
*/

#include "cod4map.h"

static const char* platform_name_pc = "pc";
static const char* platform_name_xenon = "xenon";
static const char* material_dir_pc = "materials";
static const char* material_dir_xenon = "materials";

PlatformEntry_t platformTable[2] = {
    { 0, "xenon", 0, "xenon", "materials" },   /* Xbox 360 - bigEndian=0 (needs swap) */
    { 1, "pc",    1, "pc",    "materials" }     /* PC - bigEndian=1 (no swap needed)   */
};

PlatformEntry_t *g_sourcePlatform;

char s_assertDisable_SetTargetPlatformByName;
char s_assertDisable_tpBoundsCheck;
char s_assertDisable_tpTableMatch;

/* COD4 stores the selected platform as the index into this name table:
   0 = unset, 1 = pc, 2 = xenon, 3 = ps3.  Keep the public PlatformEntry_t
   adapter used by the rest of the compiler, but retain that visible order. */
static const char *s_targetPlatformNames[] = {
    NULL, "pc", "xenon", "ps3"
};

/* PS3 uses the same big-endian BSP serialization path as Xenon, but its
   platform path must remain distinct for filesystem path replacement. */
static PlatformEntry_t s_ps3Platform = {
    PLATFORM_XENON, "ps3", 0, "ps3", "materials"
};

static PlatformEntry_t *s_targetPlatforms[] = {
    NULL,
    &platformTable[PLATFORM_PC],
    &platformTable[PLATFORM_XENON],
    &s_ps3Platform
};


/*
================
GetPlatformById

Looks up platform entry by numeric ID.
================
*/
PlatformEntry_t *GetPlatformById(unsigned int platformId)
{

  Assert(platformId < PLATFORM_COUNT, s_assertDisable_tpBoundsCheck);
  Assert(platformTable[platformId].platformId == (int)platformId, s_assertDisable_tpTableMatch);

  return &platformTable[platformId];
}

/*
================
SetTargetPlatformByName

Sets the active target platform by name string.
================
*/
int SetTargetPlatformByName(char *platformName)
{
  int i;

  Assert(platformName, s_assertDisable_SetTargetPlatformByName);

  for (i = 0; i < (int)(sizeof(s_targetPlatformNames) / sizeof(s_targetPlatformNames[0])); i++)
  {
    if (s_targetPlatformNames[i] && _stricmp(platformName, s_targetPlatformNames[i]) == 0)
    {
      if (!g_targetPlatform || g_targetPlatform == s_targetPlatforms[i])
      {
        g_targetPlatform = s_targetPlatforms[i];
        return 1;
      }
      else
      {
        Com_Error(
          "Error: Target platform already set as %s, trying to set again as %s\n",
          g_targetPlatform->name,
          s_targetPlatformNames[i]);
        return 0;
      }
    }
  }
  return 0;
}

/*
================
ValidatePlatformSet

Validates that a target platform has been set.
Prints available platforms if none is set.
================
*/
int ValidatePlatformSet(void)
{
  int i;

  if ( g_targetPlatform )
    return 1;
  printf("No platform specified.  '-platform' must be set using one of the following:\n");
  for (i = 0; i < (int)(sizeof(s_targetPlatformNames) / sizeof(s_targetPlatformNames[0])); i++)
  {
    if (s_targetPlatformNames[i])
      printf("  %s\n", s_targetPlatformNames[i]);
  }
  return 0;
}

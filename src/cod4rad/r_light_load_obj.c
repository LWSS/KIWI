/*
 * r_light_load_obj.c — load a lights/<name> attenuation image.
 *
 * Source: C:\trees\cod3\cod3src\src\gfx_d3d\r_light_load_obj.cpp
 */

#include "cod2rad64.h"
#include <string.h>

static char s_assertDisable_LoadLightDefImages_name;

/*
================
LoadLightDefImages

Loads the attenuation image named by a `lights/<name>` asset. Native
sub_454610 reads a one-byte sampler state followed immediately by one
nul-terminated image name. Rad does not retain the sampler state.

Returns 1 on success, 0 on a missing or empty light-def file.
================
*/
int LoadLightDefImages(const char *name, ImageDecodeState_t *outImage)
{
    void *buffer;
    int len;
    const char *imageName;

    Assert(name, s_assertDisable_LoadLightDefImages_name);

    len = FS_ReadFile(va("lights/%s", name), &buffer);
    if (len < 0)
        return 0;
    if (len == 0)
    {
        FS_FreeFile(buffer);
        return 0;
    }

    imageName = (const char *)buffer + 1;
    if (*imageName)
        Image_LoadIWI(imageName, outImage);
    else
        memset(outImage, 0, sizeof(*outImage));

    FS_FreeFile(buffer);
    return 1;
}

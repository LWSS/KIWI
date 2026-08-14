/*
 * LoadLightDefImages.c — load a lights/<name> asset pair.
 *
 * Source: ..\src\gfx_d3d\LoadLightDefImages.cpp
 */

#include "cod2rad64.h"
#include <string.h>

static char s_assertDisable_LoadLightDefImages_name;

/*
================
LoadLightDefImages

Loads a `lights/<name>` file produced by the asset compiler. The
file layout is:

    [0]  type byte (stored at *outType)
    [1]  1 byte of padding
    [2]  image-1 name, nul-terminated
    [L1+3]  1 byte of padding
    [L1+4]  image-2 name, nul-terminated

Each non-empty image name is passed to Image_LoadIWI to populate the
caller's ImageDecodeState slot; an empty name zero-fills its slot
(sizeof(ImageDecodeState_t) == 0x58).

Note the parameter order: the first name in the file targets
`outImage1` (the 4th param) and the second name targets `outImage2`
(the 3rd param) — this mirrors the compiled register usage.

Returns 1 on success, 0 on missing file or empty file.
================
*/
int LoadLightDefImages(const char *name, int *outType,
                     ImageDecodeState_t *outImage2, ImageDecodeState_t *outImage1)
{
    void *buffer;
    int len;
    const char *firstName;
    const char *secondName;
    int firstLen;
    int secondLen;

    Assert(name, s_assertDisable_LoadLightDefImages_name);

    len = FS_ReadFile(va("lights/%s", name), &buffer);
    if (len < 0)
        return 0;
    if (len == 0)
    {
        FS_FreeFile(buffer);
        return 0;
    }

    *outType = *(unsigned char *)buffer;

    firstName = (const char *)buffer + 2;
    firstLen = (int)strlen(firstName);
    if (firstLen != 0)
        Image_LoadIWI(firstName, outImage1);
    else
        memset(outImage1, 0, sizeof(ImageDecodeState_t));

    secondName = firstName + firstLen + 2;
    secondLen = (int)strlen(secondName);
    if (secondLen != 0)
        Image_LoadIWI(secondName, outImage2);
    else
        memset(outImage2, 0, sizeof(ImageDecodeState_t));

    FS_FreeFile(buffer);
    return 1;
}

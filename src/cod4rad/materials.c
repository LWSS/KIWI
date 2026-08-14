/*
 * materials.c — Material loading and caching for radiosity.
 *
 * Source: materials.cpp (from LST annotation)
 * Loads material definitions from files, caches them in a 4096-entry array.
 */

#include "cod2rad64.h"
#include <string.h>

static char s_assertDisable_LoadMaterial_platform;
static char s_assertDisable_LoadMaterial_matDir;
static char s_assertDisable_LoadMaterial_matDir0;

MaterialDef_t g_materialCache[MAX_RAD_MATERIALS];
int           g_materialCacheCount;
float         g_defaultTessSize;

extern int LoadFile(const char *path, void **fileData);
extern void FS_FreeFile(void *fileData);                                         /* com_files_427580 */
extern int sprintf_wrap(char *buf, const char *fmt, ...);
extern float FloatSwap(float v);
extern void *Z_Malloc(int size);
extern void Z_FreeInternal(void *ptr);
extern void memset_fast(void *dst, int val, int size);
/* Image_LoadIWI — in cod2rad64.h */

static char s_assertDisable_Material_ByteSwap_info;

/*
================
Material_ByteSwap

Byte-swaps the material file header fields for big-endian
target platforms (Xbox 360). Brackets the swap loop with
Swap_Init/Swap_InitByteSwap.
================
*/
void Material_ByteSwap(void *info)
{
    MaterialInfo_t *mat = (MaterialInfo_t *)info;

    Assert(info, s_assertDisable_Material_ByteSwap_info);

    Swap_Init_BigEndian();

    mat->hashIndex     = (short)ShortSwap(mat->hashIndex);
    mat->sortedIndex   = (short)ShortSwap(mat->sortedIndex);
    mat->locale        = LongSwap(mat->locale);
    mat->toolFlags     = (unsigned short)ShortSwap(mat->toolFlags);
    mat->maxDeformMove = (int)FloatSwap(*(float *)&mat->maxDeformMove);
    mat->autoTexScaleW = (unsigned short)ShortSwap(mat->autoTexScaleW);
    mat->autoTexScaleH = (unsigned short)ShortSwap(mat->autoTexScaleH);
    mat->tessSize      = FloatSwap(mat->tessSize);
    mat->surfaceFlags  = LongSwap(mat->surfaceFlags);
    mat->contents      = LongSwap(mat->contents);

    Swap_Init();
}

/*
================
Material_LoadAlphaImage

Decodes the material's reference image and builds an alpha-mask
bitmap for collision testing on masked materials. Three modes
based on toolFlags & 0x70:
  0x30 — bit set if alpha != 0
  0x40 — bit set if alpha >= 0x80
  0x50 — bit set if alpha <  0x80 (inverted)
Other modes return NULL.

Returns a packed 1-bit-per-pixel bitmap, or NULL if no alpha mask.
================
*/
void *Material_LoadAlphaImage(void *fileData, int *texScaleW, int *texScaleH)
{
    ImageDecodeState_t imageInfo;
    unsigned char *bytes = (unsigned char *)fileData;
    unsigned char *imgData;
    unsigned char *bitmap;
    int alphaMode;
    int threshold;
    unsigned int xorMask;
    int width, height;
    int totalPixels;
    int byteCount;
    int i;
    int refImageOfs;
    unsigned int alpha;
    MaterialInfo_t *matInfo = (MaterialInfo_t *)fileData;

    alphaMode = matInfo->toolFlags & 0x70;

    if (alphaMode == 0x30)
    {
        xorMask = 0;
        threshold = 1;
    }
    else if (alphaMode == 0x40)
    {
        xorMask = 0;
        threshold = 0x80;
    }
    else if (alphaMode == 0x50)
    {
        xorMask = 0xFF;
        threshold = 0x80;
    }
    else
    {
        return 0;
    }

    /* decode image: matFile + matFile->referenceImageName */
    refImageOfs = matInfo->referenceImageName;
    Image_LoadIWI((const char *)(bytes + refImageOfs), &imageInfo);

    imgData = imageInfo.pixels;
    if (!imgData)
        return 0;

    width  = imageInfo.stride;
    height = imageInfo.height;

    if (texScaleW)
        *texScaleW = width;
    if (texScaleH)
        *texScaleH = height;

    totalPixels = width * height;
    byteCount = (totalPixels + 7) / 8;

    bitmap = (unsigned char *)Z_Malloc(byteCount);
    if (!bitmap)
        Error("Couldn't allocate %i bytes for an alpha mask\n", byteCount);

    memset_fast(bitmap, 0, byteCount);

    for (i = 0; i < totalPixels; i++)
    {
        alpha = imgData[i * 4 + 3] ^ xorMask;
        if ((int)alpha >= threshold)
            bitmap[i >> 3] |= (unsigned char)(1 << (i & 7));
    }

    Z_FreeInternal(imgData);

    return bitmap;
}

/*
================
LoadMaterial

Searches a 4096-entry material cache (128 bytes per entry, inline strcmp).
If not found, loads the material file via LoadFile, parses the header
fields into a new cache entry, and returns it. Falls back to "$default"
material on load failure.
================
*/
void *LoadMaterial(const char *materialName)
{
    char filePath[1024];
    void *fileData;
    int materialCount;
    unsigned char *cache;
    unsigned char *entry;
    int i;
    const char *a, *b;
    const char *matDir;
    unsigned char *matData;
    int fileSize;
    int nonColliding;

    materialCount = g_materialCacheCount;
    cache = g_materialCache;

    /* search existing cache (inline strcmp) */
    for (i = 0; i < materialCount; i++)
    {
        a = materialName;
        b = (const char *)(cache + (long long)i * 128);
        while (*a == *b)
        {
            if (*b == '\0')
                break;
            a++;
            b++;
        }
        if (*a == *b)
        {
            /* found in cache — return entry pointer */
            return cache + (long long)i * 128;
        }
    }

    /* not found — allocate new slot */
    entry = cache + (long long)materialCount * 128;
    g_materialCacheCount = materialCount + 1;

    /* Assert(g_targetPlatform) */
    Assert(g_targetPlatform, s_assertDisable_LoadMaterial_platform);

    /* Assert(g_targetPlatform->materialDirectory) */
    matDir = g_targetPlatform->materialDirectory;
    Assert(matDir, s_assertDisable_LoadMaterial_matDir);

    /* Assert(g_targetPlatform->materialDirectory[0]) */
    Assert(matDir[0], s_assertDisable_LoadMaterial_matDir0);

    /* build file path: "materialDirectory/materialName" */
    sprintf_wrap(filePath, "%s/%s", matDir, materialName);

    /* load material file via FS search paths */
    fileSize = FS_ReadFile(filePath, &fileData);
    if (fileSize < 0)
    {
        /* file not found — try $default */
        if (strcmp(materialName, "$default") == 0)
            Error("Cannot find material '$default'");

        return LoadMaterial("$default");
    }

    /* byte-swap if platform is xenon (platformId == 0) */
    if (g_targetPlatform->platformId == 0)
        Material_ByteSwap(fileData);

    /* copy material name into cache entry (inline strcpy) */
    {
        const char *src = materialName;
        char *dst = (char *)entry;
        while ((*dst++ = *src++) != '\0')
            ;
    }

    /* parse material header fields into cache entry */
    matData = (unsigned char *)fileData;

    /* contents and surfaceFlags from file header */
    *(int *)(entry + 0x40) = *(int *)(matData + 0x24);
    *(int *)(entry + 0x44) = *(int *)(matData + 0x28);

    /* toolFlagsWord (short) */
    *(short *)(entry + 0x48) = *(short *)(matData + 0x16);

    /* (gameFlags >> 1) & 1 */
    *(int *)(entry + 0x4C) = (*(unsigned char *)(matData + 0x0C) >> 1) & 1;

    /* toolFlags & 1 */
    *(int *)(entry + 0x50) = *(unsigned char *)(matData + 0x16) & 1;

    /* tessellation size */
    *(float *)(entry + 0x58) = *(float *)(matData + 0x20);
    if (*(float *)(entry + 0x58) == 0.0f)
        *(float *)(entry + 0x58) = g_defaultTessSize;

    /* reserved field = 0 */
    *(int *)(entry + 0x5C) = 0;

    /* (toolFlags >> 7) & 1 */
    *(int *)(entry + 0x60) = (*(unsigned char *)(matData + 0x16) >> 7) & 1;

    /* (surfaceFlags >> 7) & 1 */
    *(int *)(entry + 0x6C) = (*(int *)(entry + 0x40) >> 7) & 1;

    /* nonColliding: (contentFlags byte & 4) || (toolFlags & 0x70) == 0x20 */
    nonColliding = 0;
    if (*(unsigned char *)(matData + 0x28) & 4)
        nonColliding = 1;
    else if ((*(unsigned char *)(matData + 0x16) & 0x70) == 0x20)
        nonColliding = 1;
    *(int *)(entry + 0x64) = nonColliding;

    /* auto texture scale (word → dword) */
    *(int *)(entry + 0x70) = *(unsigned short *)(matData + 0x1C);
    *(int *)(entry + 0x74) = *(unsigned short *)(matData + 0x1E);

    /* load alpha image data */
    *(void **)(entry + 0x78) = Material_LoadAlphaImage(fileData,
        (int *)(entry + 0x70), (int *)(entry + 0x74));

    /* free file data */
    FS_FreeFile(fileData);

    return entry;
}

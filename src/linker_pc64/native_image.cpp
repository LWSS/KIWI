#include <universal/q_shared.h>
#include <gfx_d3d/r_image.h>
#include <gfx_d3d/r_pixelcost_load_obj.h>
#include <database64/db_image_assets.h>
#include <database64/db_package.h>
#include "native_image.h"
#include "native_wavelet.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

static bool Fail(char *error, size_t capacity, const char *message)
{
    if (capacity)
    {
        snprintf(error, capacity, "%s", message);
    }
    return false;
}

void Linker_FreeImage(GfxImage *image)
{
    if (image)
    {
        free(image->texture.loadDef);
        free((void *)image->name);
        free(image);
    }
}

bool Linker_CreateImage(const char *name, unsigned int width, unsigned int height, int format, uint8_t flags,
                        uint8_t category, uint8_t semantic, uint8_t track, const void *pixels, size_t size,
                        GfxImage **image, char *error, size_t errorSize)
{
    if (!image || (!error && errorSize))
    {
        return false;
    }
    *image = NULL;
    if (errorSize)
    {
        error[0] = 0;
    }
    char normalized[DB64_PACKAGE_PATH];
    if (!name || !DB64_NormalizePath(name, normalized, sizeof(normalized)) || !pixels || !size || size > INT_MAX ||
        !width || width > INT16_MAX || !height || height > INT16_MAX || track >= IMAGE_TRACK_COUNT ||
        (width & (width - 1)) || (height & (height - 1)) ||
        ((flags & IMG_FLAG_VOLMAP) && (flags & IMG_FLAG_CUBEMAP)))
    {
        return Fail(error, errorSize, "Invalid generated native image dimensions, name or pixels");
    }
    GfxImage *result = (GfxImage *)calloc(1, sizeof(GfxImage));
    if (!result)
    {
        return Fail(error, errorSize, "Out of memory creating native image");
    }
    result->name = _strdup(normalized);
    result->texture.loadDef = (GfxImageLoadDef *)calloc(1, offsetof(GfxImageLoadDef, data) + size);
    if (!result->name || !result->texture.loadDef)
    {
        Linker_FreeImage(result);
        return Fail(error, errorSize, "Out of memory creating native image pixels");
    }
    result->mapType = (flags & IMG_FLAG_CUBEMAP) ? MAPTYPE_CUBE : ((flags & IMG_FLAG_VOLMAP) ? MAPTYPE_3D : MAPTYPE_2D);
    result->width = (uint16_t)width;
    result->height = (uint16_t)height;
    result->depth = 1;
    result->category = category;
    result->semantic = semantic;
    result->track = track;
    result->noPicmip = (flags & (IMG_FLAG_NOPICMIP | IMG_FLAG_NOMIPMAPS)) != 0 || width < 32 || height < 32;
    result->cardMemory.platform[0] = (int)size;
    result->cardMemory.platform[1] = (int)size;
    GfxImageLoadDef *definition = result->texture.loadDef;
    definition->flags = flags;
    definition->levelCount = 1;
    if (!(flags & IMG_FLAG_NOMIPMAPS))
    {
        for (unsigned int resolution = 1; resolution < width || resolution < height; resolution *= 2)
        {
            ++definition->levelCount;
        }
    }
    definition->dimensions[0] = (int16_t)width;
    definition->dimensions[1] = (int16_t)height;
    definition->dimensions[2] = 1;
    definition->format = (D3DFORMAT)format;
    definition->resourceSize = (int)size;
    if (!DB64_ValidateImageLayout(result, definition))
    {
        Linker_FreeImage(result);
        return Fail(error, errorSize, "Generated native image pixel count or format is invalid");
    }
    memcpy(definition->data, pixels, size);
    *image = result;
    return true;
}

bool Linker_CreateBuiltinImage(const char *name, uint8_t semantic, uint8_t track,
                               GfxImage **image, char *error, size_t errorSize)
{
    if (!image || (!error && errorSize))
    {
        return false;
    }
    *image = NULL;
    uint32_t pixel = 0;
    uint8_t flags = IMG_FLAG_NOMIPMAPS | IMG_FLAG_NOPICMIP;
    if (!name)
    {
        return Fail(error, errorSize, "Missing built-in image name");
    }
    if (!_stricmp(name, "$pixelcostcolorcode"))
    {
        uint8_t pixels[256][4];
        RB_PixelCost_BuildColorCodeMap(pixels, 256);
        return Linker_CreateImage(name, 256, 1, D3DFMT_X8R8G8B8, flags, IMG_CATEGORY_AUTO_GENERATED,
                                  semantic, track, pixels, sizeof(pixels), image, error, errorSize);
    }
    if (!_stricmp(name, "$white"))
    {
        pixel = 0xFFFFFFFF;
    }
    else if (!_stricmp(name, "$black") || !_stricmp(name, "$black_3d") || !_stricmp(name, "$black_cube"))
    {
        pixel = 0xFF000000;
        if (!_stricmp(name, "$black_3d"))
        {
            flags |= IMG_FLAG_VOLMAP;
        }
        else if (!_stricmp(name, "$black_cube"))
        {
            flags |= IMG_FLAG_CUBEMAP;
        }
    }
    else if (!_stricmp(name, "$gray"))
    {
        pixel = 0x80808080;
    }
    else if (!_stricmp(name, "$identitynormalmap"))
    {
        pixel = 0x808080FF;
    }
    else
    {
        return Fail(error, errorSize, "Unsupported built-in image constructor");
    }
    const uint32_t pixels[6] = {pixel, pixel, pixel, pixel, pixel, pixel};
    const size_t size = flags & IMG_FLAG_CUBEMAP ? sizeof(pixels) : sizeof(uint32_t);
    return Linker_CreateImage(name, 1, 1, D3DFMT_A8R8G8B8, flags, IMG_CATEGORY_AUTO_GENERATED,
                              semantic, track, pixels, size, image, error, errorSize);
}

bool Linker_ImportImage(const void *data, size_t size, const char *name, uint8_t semantic, uint8_t track,
                        GfxImage **image, char *error, size_t errorSize)
{
    if (!image || (!error && errorSize))
    {
        return false;
    }
    *image = NULL;
    if (errorSize)
    {
        error[0] = 0;
    }
    char normalized[DB64_PACKAGE_PATH];
    if (!data || size < sizeof(GfxImageFileHeader) || size > INT_MAX || !name ||
        !DB64_NormalizePath(name, normalized, sizeof(normalized)) || track >= IMAGE_TRACK_COUNT)
    {
        return Fail(error, errorSize, "Invalid IWI input size, name or image track");
    }
    GfxImageFileHeader header;
    memcpy(&header, data, sizeof(GfxImageFileHeader));
    if (memcmp(header.tag, "IWi", sizeof(header.tag)) || header.version != 6)
    {
        return Fail(error, errorSize, "Native image importer requires IWI version 6");
    }
    if (header.flags & IMG_FLAG_VOLMAP)
    {
        return Fail(error, errorSize, "IWI volume importing is not implemented yet");
    }
    if (header.dimensions[0] <= 0 || header.dimensions[1] <= 0 || header.dimensions[2] != 1 ||
        (header.dimensions[0] & (header.dimensions[0] - 1)) || (header.dimensions[1] & (header.dimensions[1] - 1)) ||
        ((header.flags & IMG_FLAG_CUBEMAP) && header.dimensions[0] != header.dimensions[1]))
    {
        return Fail(error, errorSize, "Invalid IWI texture dimensions");
    }
    if (header.format >= IMG_FORMAT_WAVELET_RGBA && header.format <= IMG_FORMAT_WAVELET_ALPHA)
    {
        return Linker_ImportWavelet(&header, (const uint8_t *)data + sizeof(GfxImageFileHeader),
                                     size - sizeof(GfxImageFileHeader), normalized, semantic, track, image, error, errorSize);
    }
    D3DFORMAT format;
    size_t bytesPerPixel = 0;
    size_t blockBytes = 0;
    switch (header.format)
    {
    case IMG_FORMAT_BITMAP_RGBA:
        format = D3DFMT_A8R8G8B8;
        bytesPerPixel = 4;
        break;
    case IMG_FORMAT_BITMAP_RGB:
        format = D3DFMT_X8R8G8B8;
        bytesPerPixel = 3;
        break;
    case IMG_FORMAT_BITMAP_LUMINANCE_ALPHA:
        format = D3DFMT_A8L8;
        bytesPerPixel = 2;
        break;
    case IMG_FORMAT_BITMAP_LUMINANCE:
        format = D3DFMT_L8;
        bytesPerPixel = 1;
        break;
    case IMG_FORMAT_BITMAP_ALPHA:
        format = D3DFMT_A8;
        bytesPerPixel = 1;
        break;
    case IMG_FORMAT_DXT1:
        format = D3DFMT_DXT1;
        blockBytes = 8;
        break;
    case IMG_FORMAT_DXT3:
        format = D3DFMT_DXT3;
        blockBytes = 16;
        break;
    case IMG_FORMAT_DXT5:
        format = D3DFMT_DXT5;
        blockBytes = 16;
        break;
    default:
        return Fail(error, errorSize, "Unsupported IWI format (DXN importing is not implemented yet)");
    }
    uint mipCount = 1;
    if (!(header.flags & IMG_FLAG_NOMIPMAPS))
    {
        for (uint resolution = 1; resolution < (uint)header.dimensions[0] || resolution < (uint)header.dimensions[1];
             resolution *= 2)
        {
            ++mipCount;
        }
    }
    const size_t faces = header.flags & IMG_FLAG_CUBEMAP ? 6 : 1;
    size_t sourceSizes[16] = {};
    size_t outputSizes[16] = {};
    size_t outputOffsets[16] = {};
    size_t sourceTotal = 0;
    size_t outputPerFace = 0;
    for (uint mip = 0; mip < mipCount; ++mip)
    {
        const size_t width = header.dimensions[0] >> mip ? header.dimensions[0] >> mip : 1;
        const size_t height = header.dimensions[1] >> mip ? header.dimensions[1] >> mip : 1;
        sourceSizes[mip] =
            blockBytes ? ((width + 3) / 4) * ((height + 3) / 4) * blockBytes : width * height * bytesPerPixel;
        outputSizes[mip] = bytesPerPixel == 3 ? width * height * 4 : sourceSizes[mip];
        outputOffsets[mip] = outputPerFace;
        outputPerFace += outputSizes[mip];
        sourceTotal += sourceSizes[mip] * faces;
    }
    const size_t resourceSize = outputPerFace * faces;
    if (sourceTotal != size - sizeof(GfxImageFileHeader) || resourceSize > INT_MAX - sizeof(GfxImageLoadDef))
    {
        return Fail(error, errorSize, "IWI pixel data does not match its mip layout");
    }
    for (uint picmip = 0; picmip < ARRAY_COUNT(header.fileSizeForPicmip); ++picmip)
    {
        const uint firstMip = picmip < mipCount ? picmip : mipCount - 1;
        size_t expected = sizeof(GfxImageFileHeader);
        for (uint mip = firstMip; mip < mipCount; ++mip)
        {
            expected += sourceSizes[mip] * faces;
        }
        if (header.fileSizeForPicmip[picmip] < 0 || (size_t)header.fileSizeForPicmip[picmip] != expected)
        {
            return Fail(error, errorSize, "Invalid IWI picmip size table");
        }
    }
    GfxImage *result = (GfxImage *)calloc(1, sizeof(GfxImage));
    if (!result)
    {
        return Fail(error, errorSize, "Out of memory allocating native image");
    }
    result->name = _strdup(normalized);
    result->texture.loadDef = (GfxImageLoadDef *)calloc(1, offsetof(GfxImageLoadDef, data) + resourceSize);
    if (!result->name || !result->texture.loadDef)
    {
        Linker_FreeImage(result);
        return Fail(error, errorSize, "Out of memory importing IWI pixels");
    }
    result->mapType = faces == 6 ? MAPTYPE_CUBE : MAPTYPE_2D;
    result->width = header.dimensions[0];
    result->height = header.dimensions[1];
    result->depth = 1;
    result->semantic = semantic;
    result->track = track;
    result->category = IMG_CATEGORY_RAW;
    result->noPicmip =
        (header.flags & (IMG_FLAG_NOPICMIP | IMG_FLAG_NOMIPMAPS)) != 0 || result->width < 32 || result->height < 32;
    result->cardMemory.platform[0] = (int)resourceSize;
    result->cardMemory.platform[1] = (int)resourceSize;
    GfxImageLoadDef *definition = result->texture.loadDef;
    definition->levelCount = (uint8_t)mipCount;
    definition->flags = header.flags;
    memcpy(definition->dimensions, header.dimensions, sizeof(short[3]));
    definition->format = format;
    definition->resourceSize = (int)resourceSize;
    const uint8_t *source = (const uint8_t *)data + sizeof(GfxImageFileHeader);
    // IWI stores smallest mip first, with all faces adjacent at each level.
    // Load_Texture consumes one complete face at a time, largest mip first.
    for (int mip = (int)mipCount - 1; mip >= 0; --mip)
    {
        for (size_t face = 0; face < faces; ++face)
        {
            uint8_t *output = definition->data + face * outputPerFace + outputOffsets[mip];
            if (bytesPerPixel == 3)
            {
                for (size_t pixel = 0; pixel < sourceSizes[mip] / 3; ++pixel)
                {
                    memcpy(output + pixel * 4, source + pixel * 3, sizeof(uint8_t[3]));
                    output[pixel * 4 + 3] = 255;
                }
            }
            else
            {
                memcpy(output, source, sourceSizes[mip]);
            }
            source += sourceSizes[mip];
        }
    }
    if (!DB64_ValidateImageLayout(result, definition))
    {
        Linker_FreeImage(result);
        return Fail(error, errorSize, "Imported IWI is not a supported native image layout");
    }
    *image = result;
    return true;
}

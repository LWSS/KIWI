#include <universal/q_shared.h>
#include <gfx_d3d/r_image.h>
#include <database64/db_package.h>
#include <database64/db_image_assets.h>
#include "native_asset_files.h"
#include "native_image.h"
#include "native_light_files.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void Linker_FreeLightDefinition(GfxLightDef *definition)
{
    if (definition)
    {
        Linker_FreeImage(definition->attenuation.image);
        free((void *)definition->name);
        free(definition);
    }
}

bool Linker_CompileLightDefinition(const char *root, const char *name, unsigned int lookupStart,
                                   GfxLightDef **definition, char *error, size_t errorSize)
{
    if (!definition || (!error && errorSize))
    {
        return false;
    }
    *definition = NULL;
    if (errorSize)
    {
        error[0] = 0;
    }
    char normalized[1024], path[2048], imageName[1024];
    bool valid = root && name && lookupStart >= 1 && lookupStart < 511 &&
                 DB64_NormalizePath(name, normalized, sizeof(normalized));
    void *data = NULL;
    size_t size = 0;
    if (valid)
    {
        snprintf(path, sizeof(path), "lights/%s", normalized);
        valid = Linker_ReadRawAssetFile(root, path, &data, &size, 1025) && size >= 3 &&
                ((const char *)data)[size - 1] == 0 && !memchr((const char *)data + 1, 0, size - 2) &&
                DB64_NormalizePath((const char *)data + 1, imageName, sizeof(imageName));
    }
    GfxLightDef *result = valid ? (GfxLightDef *)calloc(1, sizeof(GfxLightDef)) : NULL;
    valid = valid && result;
    if (valid)
    {
        result->name = _strdup(normalized);
        result->attenuation.samplerState = *(const uint8_t *)data;
        result->lmapLookupStart = (int)lookupStart;
        valid = result->name != NULL;
    }
    free(data);
    data = NULL;
    if (valid)
    {
        snprintf(path, sizeof(path), "images/%s.iwi", imageName);
        valid = Linker_ReadRawAssetFile(root, path, &data, &size) &&
                Linker_ImportImage(data, size, imageName, TS_FUNCTION, IMAGE_TRACK_LIGHT, &result->attenuation.image,
                                   error, errorSize);
        if (valid)
        {
            const GfxImage *image = result->attenuation.image;
            valid = image->mapType == MAPTYPE_2D && image->width > 0 && lookupStart + image->width < 512;
        }
    }
    free(data);
    if (!valid)
    {
        Linker_FreeLightDefinition(result);
        if (errorSize && !error[0])
        {
            snprintf(error, errorSize,
                     "Cannot compile light '%s': invalid source, missing image or falloff row exceeds 512 pixels",
                     name ? name : "");
        }
        return false;
    }
    *definition = result;
    return true;
}

bool Linker_StampLightAttenuation(const GfxLightDef *definition, GfxImage *secondary, char *error, size_t errorSize)
{
    if (!error && errorSize)
    {
        return false;
    }
    const GfxImage *image = definition ? definition->attenuation.image : NULL;
    bool valid = image && secondary && DB64_ValidateImageLayout(image, image->texture.loadDef) &&
                 DB64_ValidateImageLayout(secondary, secondary->texture.loadDef) && image->mapType == MAPTYPE_2D &&
                 secondary->mapType == MAPTYPE_2D && secondary->width == 512 && secondary->height == 1024 &&
                 secondary->texture.loadDef->format == D3DFMT_A8R8G8B8 && definition->lmapLookupStart >= 1 &&
                 (unsigned int)definition->lmapLookupStart + image->width < 512;
    unsigned int stride = 0;
    if (valid)
    {
        switch (image->texture.loadDef->format)
        {
        case D3DFMT_A8R8G8B8:
        case D3DFMT_X8R8G8B8:
            stride = 4;
            break;
        case D3DFMT_A8L8:
            stride = 2;
            break;
        case D3DFMT_L8:
        case D3DFMT_A8:
            stride = 1;
            break;
        default:
            valid = false;
            break;
        }
    }
    if (!valid)
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Unsupported attenuation pixels or invalid secondary lightmap layout");
        }
        return false;
    }
    const uint8_t *source = image->texture.loadDef->data;
    uint8_t *destination = secondary->texture.loadDef->data + 4 * (definition->lmapLookupStart - 1);
    for (unsigned int i = 0; i < (unsigned int)image->width + 2; ++i)
    {
        unsigned int x = i ? i - 1 : 0;
        if (x >= image->width)
        {
            x = image->width - 1;
        }
        const uint8_t *pixel = source + x * stride;
        const D3DFORMAT format = image->texture.loadDef->format;
        if (stride == 4)
        {
            memcpy(destination, pixel, 4);
            if (format == D3DFMT_X8R8G8B8)
            {
                destination[3] = 255;
            }
        }
        else
        {
            destination[0] = destination[1] = destination[2] = format == D3DFMT_A8 ? 255 : pixel[0];
            destination[3] = format == D3DFMT_A8 ? pixel[0] : (stride == 2 ? pixel[1] : 255);
        }
        destination += 4;
    }
    return true;
}

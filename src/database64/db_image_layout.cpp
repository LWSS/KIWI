#include <universal/q_shared.h>
#include <gfx_d3d/r_image.h>
#include "db_image_assets.h"
#include <limits.h>

bool DB64_ValidateImageLayout(const GfxImage *image, const GfxImageLoadDef *definition)
{
    if (!image || !definition || !image->name || image->track >= IMAGE_TRACK_COUNT || definition->resourceSize < 0 ||
        definition->dimensions[0] <= 0 || definition->dimensions[1] <= 0 || definition->dimensions[2] <= 0 ||
        image->width != definition->dimensions[0] || image->height != definition->dimensions[1] ||
        image->depth != definition->dimensions[2])
    {
        return false;
    }
    const int mapFlags = definition->flags & (IMG_FLAG_CUBEMAP | IMG_FLAG_VOLMAP);
    if ((image->mapType == MAPTYPE_2D && (mapFlags || image->depth != 1)) ||
        (image->mapType == MAPTYPE_CUBE &&
         (mapFlags != IMG_FLAG_CUBEMAP || image->width != image->height || image->depth != 1)) ||
        (image->mapType == MAPTYPE_3D && mapFlags != IMG_FLAG_VOLMAP) ||
        (image->mapType != MAPTYPE_2D && image->mapType != MAPTYPE_CUBE && image->mapType != MAPTYPE_3D))
    {
        return false;
    }
    uint mipCount = 1;
    if (!(definition->flags & IMG_FLAG_NOMIPMAPS))
    {
        for (uint resolution = 1; resolution < image->width || resolution < image->height || resolution < image->depth;
             resolution *= 2)
        {
            ++mipCount;
        }
    }
    if (definition->levelCount && definition->levelCount != mipCount)
    {
        return false;
    }
    size_t total = 0;
    for (uint mip = 0; mip < mipCount; ++mip)
    {
        const size_t width = image->width >> mip ? image->width >> mip : 1;
        const size_t height = image->height >> mip ? image->height >> mip : 1;
        const size_t depth = image->depth >> mip ? image->depth >> mip : 1;
        size_t bytes;
        switch (definition->format)
        {
        case D3DFMT_DXT1:
            bytes = 8 * depth * ((height + 3) / 4) * ((width + 3) / 4);
            break;
        case D3DFMT_DXT3:
        case D3DFMT_DXT5:
            bytes = 16 * depth * ((height + 3) / 4) * ((width + 3) / 4);
            break;
        case D3DFMT_A8R8G8B8:
        case D3DFMT_X8R8G8B8:
        case D3DFMT_D24S8:
        case D3DFMT_G16R16F:
        case D3DFMT_R32F:
            bytes = 4 * width * height * depth;
            break;
        case D3DFMT_D16:
        case D3DFMT_A8L8:
            bytes = 2 * width * height * depth;
            break;
        case D3DFMT_A8:
        case D3DFMT_L8:
            bytes = width * height * depth;
            break;
        default:
            return false;
        }
        total += bytes;
        if (total > INT_MAX)
        {
            return false;
        }
    }
    if (image->mapType == MAPTYPE_CUBE)
    {
        total *= 6;
    }
    if (total > INT_MAX)
    {
        return false;
    }
    // Water textures are generated at runtime. Other native images must carry
    // every mip/face; an external .iwi fallback would defeat native linking.
    return (definition->resourceSize == 0 && image->category == IMG_CATEGORY_WATER && image->mapType == MAPTYPE_2D) ||
           (size_t)definition->resourceSize == total;
}
